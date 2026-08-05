/* wav_output.c -- decode track data to a PCM .wav file on the fly.
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
 * Decoders: MP3 via minimp3 (public domain), AAC via faad2 (HAVE_FAAD), and
 * Ogg Vorbis via libvorbis (OGG_VORBIS_FOUND).  All produce 16-bit PCM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "srtypes.h"
#include "errors.h"
#include "wav_output.h"
#include "debug.h"

#include <unistd.h>
#include "minimp3.h"
#if defined (HAVE_FAAD)
#include <neaacdec.h>
#endif
#if OGG_VORBIS_FOUND
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#endif

#define WAV_HEADER_SIZE 44

typedef struct wav_encoder Wav_encoder;
struct wav_encoder {
    FHANDLE        fp;
    int            content_type;
    unsigned char *leftover;      /* undecoded tail carried between writes (mp3/aac) */
    unsigned long  leftover_len;
    unsigned int   samplerate;
    unsigned int   channels;
    int            have_format;
    unsigned long  data_bytes;    /* PCM bytes written so far */

    mp3dec_t       mp3d;
#if defined (HAVE_FAAD)
    NeAACDecHandle aac;
    int            aac_inited;
#endif
#if OGG_VORBIS_FOUND
    ogg_sync_state   oy;
    ogg_stream_state os;
    vorbis_info      vi;
    vorbis_comment   vc;
    vorbis_dsp_state vd;
    vorbis_block     vb;
    int              vorbis_os_inited;
    int              vorbis_headers;      /* header packets seen (need 3) */
    int              vorbis_synth_inited;
#endif
};

static void
put_u32_le (unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char) (v & 0xff);
    p[1] = (unsigned char) ((v >> 8) & 0xff);
    p[2] = (unsigned char) ((v >> 16) & 0xff);
    p[3] = (unsigned char) ((v >> 24) & 0xff);
}

static void
put_u16_le (unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char) (v & 0xff);
    p[1] = (unsigned char) ((v >> 8) & 0xff);
}

/* Build a 44-byte canonical PCM WAV header. */
static void
build_header (unsigned char *h, unsigned int rate, unsigned int channels,
	      unsigned long data_bytes)
{
    unsigned int  bits = 16;
    unsigned int  block_align = channels * (bits / 8);
    unsigned long byte_rate = (unsigned long) rate * block_align;

    memcpy (h + 0, "RIFF", 4);
    put_u32_le (h + 4, 36 + data_bytes);
    memcpy (h + 8, "WAVE", 4);
    memcpy (h + 12, "fmt ", 4);
    put_u32_le (h + 16, 16);
    put_u16_le (h + 20, 1);             /* PCM */
    put_u16_le (h + 22, channels);
    put_u32_le (h + 24, rate);
    put_u32_le (h + 28, byte_rate);
    put_u16_le (h + 32, block_align);
    put_u16_le (h + 34, bits);
    memcpy (h + 36, "data", 4);
    put_u32_le (h + 40, data_bytes);
}

/* Rewrite the 44-byte header in place with the current format/size, then
   restore the file position.  Called after every chunk so an interrupted
   track still yields a valid, playable .wav. */
static void
patch_header (Wav_encoder *w)
{
    unsigned char hdr[WAV_HEADER_SIZE];
    off_t cur;

    if (!w->have_format)
	return;
    cur = lseek (w->fp, 0, SEEK_CUR);
    if (cur == (off_t) -1)
	return;
    build_header (hdr, w->samplerate, w->channels, w->data_bytes);
    if (lseek (w->fp, 0, SEEK_SET) != (off_t) -1) {
	if (write (w->fp, hdr, sizeof (hdr)) == -1)
	    debug_printf ("wav: header patch write failed\n");
    }
    lseek (w->fp, cur, SEEK_SET);
}

static void
set_format (Wav_encoder *w, unsigned int rate, unsigned int channels)
{
    if (!w->have_format) {
	w->samplerate = rate;
	w->channels = channels ? channels : 1;
	w->have_format = 1;
    }
}

/* Write interleaved 16-bit PCM (samples-per-channel * channels values) as
   little-endian, regardless of host byte order. */
static void
write_pcm_s16 (Wav_encoder *w, const int16_t *pcm, long n)
{
    unsigned char stackbuf[8192];
    unsigned char *out = stackbuf;
    long need = n * 2;
    long i;

    if (n <= 0)
	return;
    if (need > (long) sizeof (stackbuf)) {
	out = (unsigned char *) malloc (need);
	if (!out)
	    return;
    }
    for (i = 0; i < n; i++)
	put_u16_le (out + i*2, (unsigned int) ((unsigned short) pcm[i]));
    if (write (w->fp, out, (size_t) need) != -1)
	w->data_bytes += (unsigned long) need;
    if (out != stackbuf)
	free (out);
}

/*****************************************************************************
 * MP3 (minimp3)
 *****************************************************************************/
static unsigned long
decode_mp3 (Wav_encoder *w, unsigned char *buf, unsigned long len)
{
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    unsigned long pos = 0;

    while (pos < len) {
	mp3dec_frame_info_t info;
	int samples = mp3dec_decode_frame (&w->mp3d, buf + pos,
					   (int)(len - pos), pcm, &info);
	if (info.frame_bytes == 0)
	    break;
	if (samples > 0) {
	    int ch = info.channels ? info.channels : 1;
	    set_format (w, (unsigned int) info.hz, (unsigned int) ch);
	    write_pcm_s16 (w, pcm, (long) samples * ch);
	}
	pos += info.frame_bytes;
    }
    return len - pos;
}

/*****************************************************************************
 * AAC (faad2)
 *****************************************************************************/
#if defined (HAVE_FAAD)
static long
adts_find_sync (const unsigned char *buf, long size, long start)
{
    long i;
    for (i = start; i + 1 < size; i++)
	if (buf[i] == 0xFF && (buf[i+1] & 0xF6) == 0xF0)
	    return i;
    return -1;
}

static unsigned long
decode_aac (Wav_encoder *w, unsigned char *buf, unsigned long len)
{
    long pos, synced;

    /* Initialize on the first ADTS frame. */
    if (!w->aac_inited) {
	unsigned long rate = 0;
	unsigned char ch = 0;
	long rc;
	synced = adts_find_sync (buf, (long) len, 0);
	if (synced < 0)
	    return len;                       /* wait for a sync word */
	rc = NeAACDecInit (w->aac, buf + synced, (unsigned long)(len - synced),
			   &rate, &ch);
	if (rc < 0)
	    return 0;                         /* give up on this buffer */
	w->aac_inited = 1;
	pos = synced + rc;
    } else {
	pos = 0;
    }

    while (pos < (long) len) {
	NeAACDecFrameInfo fi;
	void *out;
	long framepos = pos;
	out = NeAACDecDecode (w->aac, &fi, buf + pos, (unsigned long)(len - pos));
	if (fi.error != 0) {
	    long nxt = adts_find_sync (buf, (long) len, pos + 1);
	    if (nxt < 0)
		return 0;                     /* drop the rest, resync next time */
	    pos = nxt;
	    continue;
	}
	if (fi.bytesconsumed == 0) {
	    /* Not enough bytes for a full frame -> carry the tail. */
	    return (unsigned long) ((long) len - framepos);
	}
	if (out && fi.samples > 0) {
	    set_format (w, (unsigned int) fi.samplerate,
			(unsigned int) fi.channels);
	    write_pcm_s16 (w, (const int16_t *) out, (long) fi.samples);
	}
	pos += (long) fi.bytesconsumed;
    }
    return 0;
}
#endif /* HAVE_FAAD */

/*****************************************************************************
 * Ogg Vorbis (libvorbis)
 *****************************************************************************/
#if OGG_VORBIS_FOUND
static void
vorbis_emit_pcm (Wav_encoder *w)
{
    float **pcm;
    int samples;

    while ((samples = vorbis_synthesis_pcmout (&w->vd, &pcm)) > 0) {
	int ch = w->vi.channels;
	int16_t *inter = (int16_t *) malloc ((size_t) samples * ch * sizeof (int16_t));
	int i, c;
	if (!inter) {
	    vorbis_synthesis_read (&w->vd, samples);
	    continue;
	}
	for (i = 0; i < samples; i++) {
	    for (c = 0; c < ch; c++) {
		float v = pcm[c][i];
		int s;
		if (v > 1.0f) v = 1.0f;
		else if (v < -1.0f) v = -1.0f;
		s = (int) lrintf (v * 32767.0f);
		inter[i*ch + c] = (int16_t) s;
	    }
	}
	write_pcm_s16 (w, inter, (long) samples * ch);
	free (inter);
	vorbis_synthesis_read (&w->vd, samples);
    }
}

static void
decode_vorbis (Wav_encoder *w, const unsigned char *buf, unsigned long len)
{
    char *ob;
    ogg_page og;
    ogg_packet op;

    ob = ogg_sync_buffer (&w->oy, (long) len);
    memcpy (ob, buf, len);
    ogg_sync_wrote (&w->oy, (long) len);

    while (ogg_sync_pageout (&w->oy, &og) == 1) {
	if (!w->vorbis_os_inited) {
	    ogg_stream_init (&w->os, ogg_page_serialno (&og));
	    vorbis_info_init (&w->vi);
	    vorbis_comment_init (&w->vc);
	    w->vorbis_os_inited = 1;
	    w->vorbis_headers = 0;
	}
	if (ogg_stream_pagein (&w->os, &og) < 0)
	    continue;
	while (ogg_stream_packetout (&w->os, &op) == 1) {
	    if (w->vorbis_headers < 3) {
		if (vorbis_synthesis_headerin (&w->vi, &w->vc, &op) < 0)
		    continue;                 /* not a vorbis header */
		if (++w->vorbis_headers == 3) {
		    if (vorbis_synthesis_init (&w->vd, &w->vi) == 0) {
			vorbis_block_init (&w->vd, &w->vb);
			w->vorbis_synth_inited = 1;
			set_format (w, (unsigned int) w->vi.rate,
				    (unsigned int) w->vi.channels);
		    }
		}
		continue;
	    }
	    if (w->vorbis_synth_inited
		&& vorbis_synthesis (&w->vb, &op) == 0) {
		vorbis_synthesis_blockin (&w->vd, &w->vb);
		vorbis_emit_pcm (w);
	    }
	}
    }
}
#endif /* OGG_VORBIS_FOUND */

/*****************************************************************************
 * Public API
 *****************************************************************************/
error_code
wav_encoder_open (void **handle, FHANDLE fp, int content_type)
{
    Wav_encoder *w;
    unsigned char hdr[WAV_HEADER_SIZE];

    if (!handle)
	return SR_ERROR_INVALID_PARAM;

    /* Reject codecs we can't decode in this build up front, so filelib can
       fall back to writing the native format. */
    switch (content_type) {
    case CONTENT_TYPE_MP3:
	break;
#if defined (HAVE_FAAD)
    case CONTENT_TYPE_AAC:
	break;
#endif
#if OGG_VORBIS_FOUND
    case CONTENT_TYPE_OGG:
	break;
#endif
    default:
	return SR_ERROR_INVALID_PARAM;
    }

    w = (Wav_encoder *) calloc (1, sizeof (Wav_encoder));
    if (!w)
	return SR_ERROR_CANT_ALLOC_MEMORY;
    w->fp = fp;
    w->content_type = content_type;

    if (content_type == CONTENT_TYPE_MP3)
	mp3dec_init (&w->mp3d);
#if defined (HAVE_FAAD)
    else if (content_type == CONTENT_TYPE_AAC) {
	NeAACDecConfigurationPtr conf;
	w->aac = NeAACDecOpen ();
	if (!w->aac) { free (w); return SR_ERROR_CANT_ALLOC_MEMORY; }
	conf = NeAACDecGetCurrentConfiguration (w->aac);
	conf->outputFormat = FAAD_FMT_16BIT;
	NeAACDecSetConfiguration (w->aac, conf);
    }
#endif
#if OGG_VORBIS_FOUND
    else if (content_type == CONTENT_TYPE_OGG)
	ogg_sync_init (&w->oy);
#endif

    memset (hdr, 0, sizeof (hdr));
    if (write (fp, hdr, sizeof (hdr)) == -1) {
	wav_encoder_close (w, fp);   /* frees decoder state */
	return SR_ERROR_CANT_WRITE_TO_FILE;
    }

    *handle = w;
    return SR_SUCCESS;
}

error_code
wav_encoder_write (void *handle, const char *buf, unsigned long size)
{
    Wav_encoder *w = (Wav_encoder *) handle;

    if (!w || !buf || size == 0)
	return SR_SUCCESS;

#if OGG_VORBIS_FOUND
    if (w->content_type == CONTENT_TYPE_OGG) {
	decode_vorbis (w, (const unsigned char *) buf, size);
	patch_header (w);
	return SR_SUCCESS;
    }
#endif

    /* mp3/aac: accumulate leftover + new bytes, decode whole frames, carry
       the undecoded tail. */
    {
	unsigned char *acc;
	unsigned long acclen, remain;

	acclen = w->leftover_len + size;
	acc = (unsigned char *) malloc (acclen);
	if (!acc)
	    return SR_ERROR_CANT_ALLOC_MEMORY;
	if (w->leftover_len)
	    memcpy (acc, w->leftover, w->leftover_len);
	memcpy (acc + w->leftover_len, buf, size);

	remain = size;   /* default: keep everything if no decoder ran */
	if (w->content_type == CONTENT_TYPE_MP3)
	    remain = decode_mp3 (w, acc, acclen);
#if defined (HAVE_FAAD)
	else if (w->content_type == CONTENT_TYPE_AAC)
	    remain = decode_aac (w, acc, acclen);
#endif

	free (w->leftover);
	w->leftover = NULL;
	w->leftover_len = 0;
	if (remain > 0 && remain <= acclen) {
	    w->leftover = (unsigned char *) malloc (remain);
	    if (!w->leftover) { free (acc); return SR_ERROR_CANT_ALLOC_MEMORY; }
	    memcpy (w->leftover, acc + acclen - remain, remain);
	    w->leftover_len = remain;
	}
	free (acc);
    }
    patch_header (w);
    return SR_SUCCESS;
}

error_code
wav_encoder_close (void *handle, FHANDLE fp)
{
    Wav_encoder *w = (Wav_encoder *) handle;

    (void) fp;
    if (!w)
	return SR_SUCCESS;

    /* Flush any remaining tail for the frame-based codecs. */
    if (w->leftover_len > 0) {
	if (w->content_type == CONTENT_TYPE_MP3)
	    decode_mp3 (w, w->leftover, w->leftover_len);
#if defined (HAVE_FAAD)
	else if (w->content_type == CONTENT_TYPE_AAC)
	    decode_aac (w, w->leftover, w->leftover_len);
#endif
    }
    patch_header (w);

#if defined (HAVE_FAAD)
    if (w->content_type == CONTENT_TYPE_AAC && w->aac)
	NeAACDecClose (w->aac);
#endif
#if OGG_VORBIS_FOUND
    if (w->content_type == CONTENT_TYPE_OGG) {
	if (w->vorbis_synth_inited) {
	    vorbis_block_clear (&w->vb);
	    vorbis_dsp_clear (&w->vd);
	}
	if (w->vorbis_os_inited) {
	    ogg_stream_clear (&w->os);
	    vorbis_comment_clear (&w->vc);
	    vorbis_info_clear (&w->vi);
	}
	ogg_sync_clear (&w->oy);
    }
#endif

    free (w->leftover);
    free (w);
    return SR_SUCCESS;
}
