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
 * The MP3 decode uses libmad; the scale()/clip logic is adapted from
 * minimad.c (distributed with libmad under the GNU GPL), as used elsewhere
 * in streamripper (see findsep.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srtypes.h"
#include "errors.h"
#include "wav_output.h"
#include "debug.h"

/* WAV output is only implemented for the POSIX file-handle (int fd) build;
   the legacy Win32 build falls back to native output (no --wav). */

#include <unistd.h>
#include "mad.h"

#define WAV_HEADER_SIZE 44

typedef struct wav_encoder Wav_encoder;
struct wav_encoder {
    FHANDLE        fp;
    struct mad_stream stream;
    struct mad_frame  frame;
    struct mad_synth  synth;
    unsigned char *leftover;      /* undecoded mp3 tail carried between writes */
    unsigned long  leftover_len;
    unsigned int   samplerate;
    unsigned int   channels;
    int            have_format;
    unsigned long  data_bytes;    /* PCM bytes written so far */
};

/* mad_fixed_t -> signed 16-bit, with rounding and clipping. */
static inline signed int
scale_16 (mad_fixed_t sample)
{
    sample += (1L << (MAD_F_FRACBITS - 16));
    if (sample >= MAD_F_ONE)
	sample = MAD_F_ONE - 1;
    else if (sample < -MAD_F_ONE)
	sample = -MAD_F_ONE;
    return sample >> (MAD_F_FRACBITS + 1 - 16);
}

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

static void
write_pcm_frame (Wav_encoder *w, struct mad_pcm *pcm)
{
    unsigned int nchannels = pcm->channels;
    unsigned int nsamples  = pcm->length;
    mad_fixed_t const *left = pcm->samples[0];
    mad_fixed_t const *right = pcm->samples[1];
    /* worst case 1152 samples * 2 ch * 2 bytes */
    unsigned char out[1152 * 2 * 2];
    unsigned char *p = out;
    unsigned int i;

    if (nsamples > 1152) nsamples = 1152;   /* defensive */

    for (i = 0; i < nsamples; i++) {
	signed int s = scale_16 (*left++);
	put_u16_le (p, (unsigned int) (s & 0xffff));
	p += 2;
	if (nchannels == 2) {
	    s = scale_16 (*right++);
	    put_u16_le (p, (unsigned int) (s & 0xffff));
	    p += 2;
	}
    }
    if (write (w->fp, out, (size_t) (p - out)) != -1)
	w->data_bytes += (unsigned long) (p - out);
}

/* Decode as many whole frames as possible from buf[0..len); return the
   number of trailing bytes that could not yet be decoded (a partial frame). */
static unsigned long
decode_buffer (Wav_encoder *w, unsigned char *buf, unsigned long len)
{
    unsigned long remain;

    mad_stream_buffer (&w->stream, buf, len);
    for (;;) {
	if (mad_frame_decode (&w->frame, &w->stream) != 0) {
	    if (w->stream.error == MAD_ERROR_BUFLEN)
		break;                        /* need more input */
	    if (MAD_RECOVERABLE (w->stream.error))
		continue;                     /* skip bad frame, keep going */
	    break;                            /* unrecoverable */
	}
	if (!w->have_format) {
	    w->samplerate = w->frame.header.samplerate;
	    w->channels = MAD_NCHANNELS (&w->frame.header);
	    w->have_format = 1;
	}
	mad_synth_frame (&w->synth, &w->frame);
	write_pcm_frame (w, &w->synth.pcm);
    }

    if (w->stream.next_frame)
	remain = (unsigned long) (buf + len - w->stream.next_frame);
    else
	remain = 0;
    return remain;
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
    mad_stream_init (&w->stream);
    mad_frame_init (&w->frame);
    mad_synth_init (&w->synth);

    /* Placeholder header; patched in wav_encoder_close(). */
    memset (hdr, 0, sizeof (hdr));
    if (write (fp, hdr, sizeof (hdr)) == -1) {
	mad_synth_finish (&w->synth);
	mad_frame_finish (&w->frame);
	mad_stream_finish (&w->stream);
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

    /* Flush the final partial frame: libmad needs MAD_BUFFER_GUARD zero
       bytes appended to decode the last frame in the stream. */
    if (w->leftover_len > 0) {
	unsigned long buflen = w->leftover_len + MAD_BUFFER_GUARD;
	unsigned char *buf = (unsigned char *) malloc (buflen);
	if (buf) {
	    memcpy (buf, w->leftover, w->leftover_len);
	    memset (buf + w->leftover_len, 0, MAD_BUFFER_GUARD);
	    decode_buffer (w, buf, buflen);
	    free (buf);
	}
    }

    /* Final header patch now that the format and total size are known. */
    patch_header (w);

    mad_synth_finish (&w->synth);
    mad_frame_finish (&w->frame);
    mad_stream_finish (&w->stream);
    free (w->leftover);
    free (w);
    return SR_SUCCESS;
}

