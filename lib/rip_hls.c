/* rip_hls.c -- record HLS (.m3u8) streams by concatenating media segments.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * HLS is unlike shoutcast/icecast: instead of one continuous byte stream, an
 * .m3u8 playlist lists discrete media segments (usually MPEG-TS with AAC).
 * For a live stream the playlist is a sliding window that we re-poll; we
 * download each newly-appearing segment and append its bytes, verbatim, to a
 * single output file.  No demux/transcode -- the result (.ts) plays in VLC,
 * ffmpeg, most players.  Segments are written as they arrive (unbuffered
 * write()), so an interrupted rip leaves a file that is valid up to the last
 * complete segment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "srtypes.h"
#include "rip_manager.h"
#include "rip_hls.h"
#include "socklib.h"
#include "errors.h"
#include "debug.h"
#include "callback.h"

#define HLS_MAX_URL      2048
#define HLS_MAX_REDIRECT 5
#define HLS_FETCH_CHUNK  32768
#define HLS_MAX_SEGMENTS 4096   /* per playlist refresh */
#define HLS_MAX_FETCH    (128 * 1024 * 1024)  /* cap a single GET (safety) */

/*****************************************************************************
 * URL helpers
 *****************************************************************************/
int
hls_url_is_m3u8 (const char *url)
{
    const char *q, *dot;
    size_t len;
    char scheme[8];

    if (!url)
	return 0;
    /* Length up to the query string. */
    q = strchr (url, '?');
    len = q ? (size_t)(q - url) : strlen (url);
    if (len >= 5 && g_ascii_strncasecmp (url + len - 5, ".m3u8", 5) == 0)
	return 1;
    /* Also treat an explicit ...m3u8 without the dot? No -- require .m3u8. */
    (void) dot; (void) scheme;
    return 0;
}

/* Case-insensitive substring test (glib's g_ascii_strncasecmp, ASCII only). */
static int
ci_contains (const char *hay, const char *needle)
{
    size_t nl = strlen (needle);
    if (!hay) return 0;
    for (; *hay; hay++)
	if (g_ascii_strncasecmp (hay, needle, nl) == 0)
	    return 1;
    return 0;
}

/* Parse "scheme://host[:port]/path".  ssl=1 for https (default port 443),
   else port 80.  Returns 0 on success. */
static int
hls_parse_url (const char *url, char *host, int hostsz, int *port,
	       char *path, int pathsz, int *ssl)
{
    const char *p = url, *s, *slash, *colon;
    size_t hlen;

    *ssl = 0;
    *port = 80;
    s = strstr (url, "://");
    if (s) {
	if ((s - url) == 5 && g_ascii_strncasecmp (url, "https", 5) == 0) {
	    *ssl = 1;
	    *port = 443;
	}
	p = s + 3;
    }
    /* authority ends at first '/' (or end of string) */
    slash = strchr (p, '/');
    hlen = slash ? (size_t)(slash - p) : strlen (p);
    if (hlen == 0 || (int) hlen >= hostsz)
	return -1;
    memcpy (host, p, hlen);
    host[hlen] = 0;
    /* split off :port */
    colon = strchr (host, ':');
    if (colon) {
	*port = atoi (colon + 1);
	if (*port <= 0)
	    return -1;
	host[colon - host] = 0;
    }
    if (slash)
	snprintf (path, pathsz, "%s", slash);
    else
	snprintf (path, pathsz, "/");
    return 0;
}

/* Resolve a playlist/segment reference against the playlist's base URL.
   Handles absolute URLs, absolute paths ("/..."), and relative paths. */
static void
hls_resolve_url (const char *base, const char *ref, char *out, int outsz)
{
    char scheme_host[HLS_MAX_URL];
    const char *authority_end;
    const char *last_slash;

    if (g_ascii_strncasecmp (ref, "http://", 7) == 0
	|| g_ascii_strncasecmp (ref, "https://", 8) == 0) {
	snprintf (out, outsz, "%s", ref);
	return;
    }

    /* scheme_host = "scheme://authority" of base */
    {
	const char *s = strstr (base, "://");
	const char *a = s ? s + 3 : base;
	authority_end = strchr (a, '/');
	if (authority_end)
	    snprintf (scheme_host, sizeof(scheme_host), "%.*s",
		      (int)(authority_end - base), base);
	else
	    snprintf (scheme_host, sizeof(scheme_host), "%s", base);
    }

    if (ref[0] == '/') {
	snprintf (out, outsz, "%s%s", scheme_host, ref);
	return;
    }

    /* relative: base directory (up to and including last '/') + ref */
    last_slash = strrchr (base, '/');
    if (last_slash && last_slash > strstr (base, "://") + 2) {
	snprintf (out, outsz, "%.*s%s", (int)(last_slash - base + 1), base, ref);
    } else {
	snprintf (out, outsz, "%s/%s", scheme_host, ref);
    }
}

/*****************************************************************************
 * HTTP GET to memory (HTTP/1.0 to avoid chunked transfer-encoding)
 *****************************************************************************/
static error_code
hls_http_get (RIP_MANAGER_INFO *rmi, const char *url,
	      char **body_out, int *bodylen_out, int depth)
{
    HSOCKET sock;
    char host[MAX_HOST_LEN], path[HLS_MAX_URL];
    int port, ssl, ret;
    char req[HLS_MAX_URL + 256];
    char header[MAX_HEADER_LEN];
    char *body = NULL;
    int cap = 0, len = 0;
    long content_length = -1;
    char *p;
    int status = 0;

    *body_out = NULL;
    *bodylen_out = 0;

    if (depth > HLS_MAX_REDIRECT)
	return SR_ERROR_INVALID_URL;
    if (hls_parse_url (url, host, sizeof(host), &port, path, sizeof(path), &ssl) != 0)
	return SR_ERROR_INVALID_URL;

    ret = socklib_open (&sock, host, port, rmi->prefs->if_name[0] ? rmi->prefs->if_name : NULL,
			rmi->prefs->timeout, ssl, GET_SSL_VERIFY (rmi->prefs->flags));
    if (ret != SR_SUCCESS)
	return ret;

    snprintf (req, sizeof(req),
	      "GET %s HTTP/1.0\r\n"
	      "Host: %s:%d\r\n"
	      "User-Agent: %s\r\n"
	      "Accept: */*\r\n"
	      "Connection: close\r\n\r\n",
	      path, host, port,
	      rmi->prefs->useragent[0] ? rmi->prefs->useragent : "Streamripper/1.x");
    ret = socklib_sendall (&sock, req, strlen(req));
    if (ret < 0) { socklib_close (&sock); return ret; }

    ret = socklib_read_header (rmi, &sock, header, sizeof(header));
    if (ret != SR_SUCCESS) { socklib_close (&sock); return ret; }

    /* status code */
    p = strchr (header, ' ');
    if (p) status = atoi (p + 1);

    /* redirect? */
    if (status >= 300 && status < 400) {
	char loc[HLS_MAX_URL], abs[HLS_MAX_URL];
	char *l = header;
	loc[0] = 0;
	while (*l) {
	    if (g_ascii_strncasecmp (l, "Location:", 9) == 0) {
		l += 9;
		while (*l == ' ') l++;
		sscanf (l, "%2047[^\r\n]", loc);
		break;
	    }
	    l++;
	}
	socklib_close (&sock);
	if (!loc[0])
	    return SR_ERROR_INVALID_URL;
	hls_resolve_url (url, loc, abs, sizeof(abs));
	return hls_http_get (rmi, abs, body_out, bodylen_out, depth + 1);
    }
    if (status != 200) {
	debug_printf ("hls_http_get: HTTP status %d for %s\n", status, url);
	socklib_close (&sock);
	return SR_ERROR_HTTP_404_ERROR;
    }

    /* Content-Length (case-insensitive) */
    {
	char *cl = header;
	while (*cl) {
	    if (g_ascii_strncasecmp (cl, "Content-Length:", 15) == 0) {
		content_length = atol (cl + 15);
		break;
	    }
	    cl++;
	}
    }

    /* Read the body. */
    for (;;) {
	int want, got;
	if (!rmi->started) { free (body); socklib_close (&sock); return SR_ERROR_ABORT_PIPE_SIGNALLED; }
	if (content_length >= 0 && len >= content_length)
	    break;
	if (len >= HLS_MAX_FETCH) {
	    debug_printf ("hls_http_get: hit %d-byte cap, stopping\n", HLS_MAX_FETCH);
	    break;
	}
	if (len + HLS_FETCH_CHUNK + 1 > cap) {
	    int ncap = cap ? cap * 2 : (HLS_FETCH_CHUNK * 4);
	    while (ncap < len + HLS_FETCH_CHUNK + 1) ncap *= 2;
	    char *nb = realloc (body, ncap);
	    if (!nb) { free (body); socklib_close (&sock); return SR_ERROR_CANT_ALLOC_MEMORY; }
	    body = nb; cap = ncap;
	}
	want = HLS_FETCH_CHUNK;
	if (content_length >= 0 && content_length - len < want)
	    want = (int)(content_length - len);
	got = socklib_recvall (rmi, &sock, body + len, want, rmi->prefs->timeout);
	if (got < 0) {
	    /* an abort during read is expected on shutdown */
	    if (got == SR_ERROR_ABORT_PIPE_SIGNALLED) { free (body); socklib_close (&sock); return got; }
	    break;   /* other recv error: treat what we have as the body end */
	}
	if (got == 0)
	    break;   /* connection closed */
	len += got;
	if (content_length < 0 && got < want)
	    break;   /* short read on a close-delimited body => done */
    }

    socklib_close (&sock);
    if (!body) { body = malloc (1); cap = 1; }
    body[len] = 0;
    *body_out = body;
    *bodylen_out = len;
    return SR_SUCCESS;
}

/*****************************************************************************
 * Playlist parsing
 *****************************************************************************/
static int
hls_is_master (const char *pl)
{
    return strstr (pl, "#EXT-X-STREAM-INF") != NULL;
}

/* From a master playlist, resolve the first variant's media-playlist URL.
   Returns 0 on success. */
static int
hls_pick_variant (const char *pl, const char *base, char *out, int outsz)
{
    const char *line = pl;
    int after_streaminf = 0;
    char buf[HLS_MAX_URL];

    while (*line) {
	const char *eol = strpbrk (line, "\r\n");
	size_t llen = eol ? (size_t)(eol - line) : strlen (line);
	if (llen > 0 && llen < sizeof(buf)) {
	    memcpy (buf, line, llen); buf[llen] = 0;
	    if (g_ascii_strncasecmp (buf, "#EXT-X-STREAM-INF", 17) == 0) {
		after_streaminf = 1;
	    } else if (buf[0] != '#' && after_streaminf) {
		hls_resolve_url (base, buf, out, outsz);
		return 0;
	    }
	}
	if (!eol) break;
	line = eol + 1;
    }
    return -1;
}

/*****************************************************************************
 * Output file
 *****************************************************************************/
/* Build the output path from prefs (output_directory + showfile name), make
   the directory, and open it for writing.  Returns the fd or -1. */
static int
hls_open_output (RIP_MANAGER_INFO *rmi, char *path_out, int path_sz)
{
    const char *dir = rmi->prefs->output_directory;
    const char *name = rmi->prefs->showfile_pattern;
    char base[SR_MAX_PATH];
    int fd;

    if (name && name[0]) {
	snprintf (base, sizeof(base), "%s", name);
    } else {
	/* derive from the stream/icy name or a default */
	snprintf (base, sizeof(base), "%s",
		  rmi->streamname[0] ? rmi->streamname : "hls_stream");
    }
    /* append .ts if there's no extension on the basename */
    {
	char *slash = strrchr (base, '/');
	char *bn = slash ? slash + 1 : base;
	if (!strchr (bn, '.'))
	    snprintf (base + strlen(base), sizeof(base) - strlen(base), ".ts");
    }

    if (dir && dir[0]) {
	mkdir (dir, 0777);   /* best-effort; ignore EEXIST */
	snprintf (path_out, path_sz, "%s/%s", dir, base);
    } else {
	snprintf (path_out, path_sz, "%s", base);
    }

    fd = open (path_out, O_WRONLY | O_CREAT | O_TRUNC,
	       S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    return fd;
}

/*****************************************************************************
 * Interruptible sleep
 *****************************************************************************/
static void
hls_wait (RIP_MANAGER_INFO *rmi, int seconds)
{
    int i, ticks = seconds * 5;   /* 200ms granularity */
    if (ticks < 1) ticks = 1;
    for (i = 0; i < ticks && rmi->started; i++)
	Sleep (200);
}

/*****************************************************************************
 * Content-based detection
 *****************************************************************************/
/* Probe a URL to decide whether it is an HLS playlist, using the response
   Content-Type and a bounded sniff of the body -- without reading a whole
   (possibly endless) shoutcast stream.  Uses a short-lived connection. */
static int
hls_probe (RIP_MANAGER_INFO *rmi, const char *url)
{
    HSOCKET sock;
    char host[MAX_HOST_LEN], path[HLS_MAX_URL];
    int port, ssl, ret, is_hls = 0;
    char req[HLS_MAX_URL + 256];
    char header[MAX_HEADER_LEN];
    char body[2048];

    if (hls_parse_url (url, host, sizeof(host), &port, path, sizeof(path), &ssl) != 0)
	return 0;
    if (socklib_open (&sock, host, port,
		      rmi->prefs->if_name[0] ? rmi->prefs->if_name : NULL,
		      rmi->prefs->timeout, ssl, GET_SSL_VERIFY (rmi->prefs->flags))
	!= SR_SUCCESS)
	return 0;

    snprintf (req, sizeof(req),
	      "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: %s\r\n"
	      "Accept: */*\r\nConnection: close\r\n\r\n",
	      path, host, port,
	      rmi->prefs->useragent[0] ? rmi->prefs->useragent : "Streamripper/1.x");
    if (socklib_sendall (&sock, req, strlen(req)) < 0) {
	socklib_close (&sock);
	return 0;
    }

    if (socklib_read_header (rmi, &sock, header, sizeof(header)) == SR_SUCCESS) {
	/* 1. Content-Type: every HLS MIME type contains "mpegurl"
	      (application/vnd.apple.mpegurl, application/x-mpegurl, ...). */
	if (ci_contains (header, "mpegurl")) {
	    is_hls = 1;
	} else {
	    /* 2. Sniff a bounded body prefix for an HLS-specific #EXT-X- tag
		  (plain .m3u/.pls playlists never contain these). */
	    ret = socklib_recvall (rmi, &sock, body, sizeof(body) - 1,
				   rmi->prefs->timeout);
	    if (ret > 0) {
		body[ret] = 0;
		if (strstr (body, "#EXT-X-") != NULL)
		    is_hls = 1;
	    }
	}
    }
    socklib_close (&sock);
    debug_printf ("hls_probe(%s) -> %d\n", url, is_hls);
    return is_hls;
}

/* Decide whether to record `url` as HLS.  Detection is content-based: we
   probe the server (Content-Type + body sniff for #EXT-X-) regardless of the
   URL extension.  This means a URL that merely *looks* like HLS (ends in
   .m3u8) but actually serves a standard stream is NOT treated as HLS -- it
   falls through to the normal shoutcast/icecast path.  The .m3u8 extension is
   only a hint used to prefer this probe when nothing else applies. */
int
hls_detect (RIP_MANAGER_INFO *rmi, const char *url)
{
    return hls_probe (rmi, url);
}

/*****************************************************************************
 * Main HLS rip
 *****************************************************************************/
error_code
hls_rip (RIP_MANAGER_INFO *rmi)
{
    char media_url[HLS_MAX_URL];
    char outpath[SR_MAX_PATH];
    char *pl = NULL;
    int pllen = 0;
    int fd = -1;
    long last_seq = -1;         /* highest media-sequence downloaded */
    error_code ret;
    int endlist = 0;
    unsigned long total = 0;

    /* HLS uses its own short-lived connections; keep rmi->stream_sock inert so
       rip_manager_stop()'s socklib_close() on it is a harmless no-op. */
    rmi->stream_sock.s = -1;
    rmi->stream_sock.closed = TRUE;
    rmi->stream_sock.ssl = NULL;
    rmi->stream_sock.ssl_ctx = NULL;

    snprintf (media_url, sizeof(media_url), "%s", rmi->prefs->url);

    /* Fetch the playlist; if it's a master, switch to the first variant. */
    ret = hls_http_get (rmi, media_url, &pl, &pllen, 0);
    if (ret != SR_SUCCESS)
	return ret;
    if (!strstr (pl, "#EXTM3U")) {
	/* Not actually an HLS playlist (shouldn't happen post-detection). */
	debug_printf ("HLS: %s has no #EXTM3U; not a playlist\n", media_url);
	free (pl);
	return SR_ERROR_CANT_PARSE_M3U;
    }
    if (hls_is_master (pl)) {
	char variant[HLS_MAX_URL];
	if (hls_pick_variant (pl, media_url, variant, sizeof(variant)) == 0) {
	    debug_printf ("HLS master -> variant: %s\n", variant);
	    snprintf (media_url, sizeof(media_url), "%s", variant);
	} else {
	    free (pl);
	    return SR_ERROR_CANT_PARSE_M3U;
	}
	free (pl); pl = NULL;
	ret = hls_http_get (rmi, media_url, &pl, &pllen, 0);
	if (ret != SR_SUCCESS)
	    return ret;
    }

    fd = hls_open_output (rmi, outpath, sizeof(outpath));
    if (fd < 0) {
	free (pl);
	return SR_ERROR_CANT_CREATE_FILE;
    }
    debug_printf ("HLS output: %s\n", outpath);

    /* Poll loop.  On entry, pl holds the current media playlist. */
    while (rmi->started) {
	const char *line = pl;
	long media_seq = 0, idx = 0;
	int target = 6;
	int got_mediaseq = 0;

	endlist = (strstr (pl, "#EXT-X-ENDLIST") != NULL);

	/* First pass: media sequence + target duration. */
	{
	    const char *m = strstr (pl, "#EXT-X-MEDIA-SEQUENCE:");
	    if (m) { media_seq = atol (m + strlen("#EXT-X-MEDIA-SEQUENCE:")); got_mediaseq = 1; }
	    m = strstr (pl, "#EXT-X-TARGETDURATION:");
	    if (m) { target = atoi (m + strlen("#EXT-X-TARGETDURATION:")); }
	    if (target < 1) target = 1;
	    if (!got_mediaseq) media_seq = 0;
	}

	/* Walk segment URIs (non-#, non-blank lines). */
	idx = 0;
	while (*line && rmi->started) {
	    const char *eol = strpbrk (line, "\r\n");
	    size_t llen = eol ? (size_t)(eol - line) : strlen (line);
	    if (llen > 0 && line[0] != '#') {
		char uri[HLS_MAX_URL], segurl[HLS_MAX_URL];
		long seq = media_seq + idx;
		idx++;
		if (llen < sizeof(uri)) {
		    memcpy (uri, line, llen); uri[llen] = 0;
		    if (seq > last_seq) {
			char *seg = NULL; int seglen = 0;
			hls_resolve_url (media_url, uri, segurl, sizeof(segurl));
			ret = hls_http_get (rmi, segurl, &seg, &seglen, 0);
			if (ret == SR_SUCCESS && seg && seglen > 0) {
			    if (write (fd, seg, seglen) == (ssize_t) seglen) {
				total += seglen;
				callback_post_status (rmi, RM_STATUS_RIPPING);
			    }
			}
			free (seg);
			last_seq = seq;
			if (!rmi->started) break;
		    }
		}
	    }
	    if (!eol) break;
	    line = eol + 1;
	}

	if (endlist)
	    break;   /* VOD: downloaded everything */

	/* Wait ~half the target duration, then re-fetch the playlist. */
	hls_wait (rmi, (target + 1) / 2);
	if (!rmi->started) break;

	free (pl); pl = NULL;
	ret = hls_http_get (rmi, media_url, &pl, &pllen, 0);
	if (ret != SR_SUCCESS) {
	    if (ret == SR_ERROR_ABORT_PIPE_SIGNALLED) break;
	    /* transient playlist fetch error: wait and retry while running */
	    hls_wait (rmi, 1);
	    if (!rmi->started) break;
	    ret = hls_http_get (rmi, media_url, &pl, &pllen, 0);
	    if (ret != SR_SUCCESS) break;
	}
    }

    if (fd >= 0) close (fd);
    free (pl);
    debug_printf ("HLS rip finished: %lu bytes -> %s\n", total, outpath);
    (void) endlist;
    return SR_SUCCESS;
}
