/* rip_hls.h -- record HLS (.m3u8) streams by concatenating media segments.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __RIP_HLS_H__
#define __RIP_HLS_H__

#include "srtypes.h"
#include "rip_manager.h"

/* True if the URL looks like an HLS playlist (ends in .m3u8, ignoring any
   ?query).  Fast, no network. */
int hls_url_is_m3u8 (const char *url);

/* Decide whether to record `url` as HLS.  Uses the .m3u8 extension as a fast
   path; otherwise probes the server (Content-Type + body sniff for #EXT-X-).
   May open a short-lived connection. */
int hls_detect (RIP_MANAGER_INFO *rmi, const char *url);

/* Record an HLS stream: resolve the playlist (master -> media), then poll the
   media playlist and append each new segment's bytes to a single output file
   until rmi->started goes false (stop / -l / Ctrl-C) or the playlist ends.
   The output file is written incrementally so it stays playable if the rip
   is interrupted. */
error_code hls_rip (RIP_MANAGER_INFO *rmi);

#endif /* __RIP_HLS_H__ */
