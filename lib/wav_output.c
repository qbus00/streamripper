/* wav_output.c -- decode mp3 track data to a PCM .wav file on the fly.
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
 * The MP3 decode uses minimp3 (lieff/minimp3, public domain), which outputs
 * 16-bit PCM directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "srtypes.h"
#include "errors.h"
#include "wav_output.h"
#include "debug.h"

#include <unistd.h>
#include "minimp3.h"

#define WAV_HEADER_SIZE 44

typedef struct wav_encoder Wav_encoder;
struct wav_encoder {
    FHANDLE        fp;
    mp3dec_t       mp3d;
    unsigned char *leftover;      /* undecoded mp3 tail carried between writes */
    unsigned long  leftover_len;
    unsigned int   samplerate;
    unsigned int   channels;
    int            have_format;
    unsigned long  data_bytes;    /* PCM bytes written so far */
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
    put_u32_le (h + 16, 16);            /* PCM fmt chunk size */
    put_u16_le (h + 20, 1);             /* audio format = PCM */
    put_u16_le (h + 22, channels);
    put_u32_le (h + 24, rate);
    put_u32_le (h + 28, byte_rate);
    put_u16_le (h + 32, block_align);
    put_u16_le (h + 34, bits);
    memcpy (h + 36, "data", 4);
    put_u32_le (h + 40, data_bytes);
}

/* Rewrite the 44-byte header in place with the current format/size, then
   restore the file position.  Called after every chunk so that even an
   interrupted (incomplete) track yields a valid, playable .wav. */
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

/* Write one decoded frame's PCM (samples-per-channel, interleaved int16) as
   little-endian 16-bit samples, regardless of host byte order. */
static void
write_pcm_frame (Wav_encoder *w, const mp3d_sample_t *pcm,
		 int samples, int channels)
{
    /* worst case 1152 samples * 2 ch * 2 bytes */
    unsigned char out[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    unsigned char *p = out;
    int n = samples * channels;
    int i;

    for (i = 0; i < n; i++) {
	put_u16_le (p, (unsigned int) ((unsigned short) pcm[i]));
	p += 2;
    }
    if (write (w->fp, out, (size_t) (p - out)) != -1)
	w->data_bytes += (unsigned long) (p - out);
}

/* Decode as many whole frames as possible from buf[0..len); return the
   number of trailing bytes that could not yet be decoded (a partial frame). */
static unsigned long
decode_buffer (Wav_encoder *w, unsigned char *buf, unsigned long len)
{
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    unsigned long pos = 0;

    while (pos < len) {
	mp3dec_frame_info_t info;
	int samples = mp3dec_decode_frame (&w->mp3d, buf + pos,
					   (int)(len - pos), pcm, &info);
	if (info.frame_bytes == 0)
	    break;                            /* need more input (partial tail) */
	if (samples > 0) {
	    if (!w->have_format) {
		w->samplerate = (unsigned int) info.hz;
		w->channels = (unsigned int) (info.channels ? info.channels : 1);
		w->have_format = 1;
	    }
	    write_pcm_frame (w, pcm, samples,
			     info.channels ? info.channels : 1);
	}
	pos += info.frame_bytes;
    }
    return len - pos;
}

error_code
wav_encoder_open (void **handle, FHANDLE fp)
{
    Wav_encoder *w;
    unsigned char hdr[WAV_HEADER_SIZE];

    if (!handle)
	return SR_ERROR_INVALID_PARAM;

    w = (Wav_encoder *) calloc (1, sizeof (Wav_encoder));
    if (!w)
	return SR_ERROR_CANT_ALLOC_MEMORY;

    w->fp = fp;
    mp3dec_init (&w->mp3d);

    /* Placeholder header; patched as data is written / on close. */
    memset (hdr, 0, sizeof (hdr));
    if (write (fp, hdr, sizeof (hdr)) == -1) {
	free (w);
	return SR_ERROR_CANT_WRITE_TO_FILE;
    }

    *handle = w;
    return SR_SUCCESS;
}

error_code
wav_encoder_write (void *handle, const char *mp3, unsigned long size)
{
    Wav_encoder *w = (Wav_encoder *) handle;
    unsigned char *buf;
    unsigned long buflen, remain;

    if (!w || !mp3 || size == 0)
	return SR_SUCCESS;

    buflen = w->leftover_len + size;
    buf = (unsigned char *) malloc (buflen);
    if (!buf)
	return SR_ERROR_CANT_ALLOC_MEMORY;
    if (w->leftover_len)
	memcpy (buf, w->leftover, w->leftover_len);
    memcpy (buf + w->leftover_len, mp3, size);

    remain = decode_buffer (w, buf, buflen);

    /* Carry the undecoded tail to the next call. */
    free (w->leftover);
    w->leftover = NULL;
    w->leftover_len = 0;
    if (remain > 0) {
	w->leftover = (unsigned char *) malloc (remain);
	if (!w->leftover) {
	    free (buf);
	    return SR_ERROR_CANT_ALLOC_MEMORY;
	}
	memcpy (w->leftover, buf + buflen - remain, remain);
	w->leftover_len = remain;
    }
    free (buf);
    patch_header (w);
    return SR_SUCCESS;
}

error_code
wav_encoder_close (void *handle, FHANDLE fp)
{
    Wav_encoder *w = (Wav_encoder *) handle;

    (void) fp;   /* the fd is held in w->fp */
    if (!w)
	return SR_SUCCESS;

    /* Try to decode any remaining tail as a final frame. */
    if (w->leftover_len > 0)
	decode_buffer (w, w->leftover, w->leftover_len);

    /* Final header patch now that the format and total size are known. */
    patch_header (w);

    free (w->leftover);
    free (w);
    return SR_SUCCESS;
}
