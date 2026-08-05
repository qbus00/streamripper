/* findsep.c
 * library routines for find silent points in mp3 data
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Portions are adapted from minimad.c, included with the 
 * libmad library, distributed under the GNU General Public License.
 * Copyright (C) 2000-2004 Underbit Technologies, Inc.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <stdint.h>
#include "minimp3.h"
#include "findsep.h"
#include "srtypes.h"
#include "debug.h"
#include "list.h"

#if defined (HAVE_FAAD)
#include <neaacdec.h>
#endif

#define MIN_RMS_SILENCE		100
#define MAX_RMS_SILENCE		32767 //max short
#define NUM_SILTRACKERS		30

typedef struct FRAME_LIST_struct FRAME_LIST;
struct FRAME_LIST_struct
{
    const unsigned char* m_framepos;
    long m_samples;
    long m_pcmpos;
    LIST m_list;
};

typedef struct SILENCETRACKERst
{
    long insilencecount;
    double silencevol;
    unsigned long silstart_samp;
    BOOL foundsil;
} SILENCETRACKER;

typedef struct DECODE_STRUCTst
{
    unsigned char* mpgbuf;  /* Input buffer to be checked for silence */
    long  mpgsize;          /* Size for mpgbuf */
    long len_to_sw_ms;
    long searchwindow_ms;
    long  silence_ms;
    long  silence_samples;
    unsigned long len_to_sw_start_samp;
    unsigned long len_to_sw_end_samp;
    unsigned long  pcmpos;
    long  samplerate;
    short prev_sample;
    SILENCETRACKER siltrackers[NUM_SILTRACKERS];
    LIST frame_list;
} DECODE_STRUCT;

/*****************************************************************************
 * Private functions
 *****************************************************************************/
static void init_siltrackers(SILENCETRACKER* siltrackers);
static void apply_padding (DECODE_STRUCT* ds, unsigned long silstart,
			   long padding1, long padding2,
			   u_long* pos1, u_long* pos2);
static void free_frame_list (DECODE_STRUCT* ds);
static void search_for_silence(DECODE_STRUCT *ds, double vol);

/*****************************************************************************
 * Private Vars
 *****************************************************************************/

/*****************************************************************************
 * Functions
 *****************************************************************************/
/* Initialize the shared decode/search state.  Codec-neutral: the mp3 (minimp3)
   and aac (faad2) front-ends both call this before feeding PCM. */
static void
findsep_init_ds (DECODE_STRUCT* ds, const char* buf, long size,
		 long len_to_sw, long searchwindow, long silence_length)
{
    ds->mpgbuf = (unsigned char*) buf;
    ds->mpgsize = size;
    ds->pcmpos = 0;
    ds->samplerate = 0;
    ds->prev_sample = 0;
    ds->len_to_sw_ms = len_to_sw;
    ds->searchwindow_ms = searchwindow;
    ds->silence_ms = silence_length;
    INIT_LIST_HEAD (&ds->frame_list);
    init_siltrackers (ds->siltrackers);
}

/* Latch the sample rate (from the first decoded frame/header) and derive the
   window bounds in samples.  Idempotent; the first non-zero rate wins. */
static void
findsep_set_samplerate (DECODE_STRUCT* ds, long samplerate)
{
    if (!ds->samplerate && samplerate > 0) {
	ds->samplerate = samplerate;
	ds->silence_samples = ds->silence_ms * (ds->samplerate/1000);
	ds->len_to_sw_start_samp = ds->len_to_sw_ms * (ds->samplerate/1000);
	ds->len_to_sw_end_samp = (ds->len_to_sw_ms + ds->searchwindow_ms)
		* (ds->samplerate/1000);
	debug_printf ("Setting samplerate: %ld\n", ds->samplerate);
    }
}

/* Feed one 16-bit mono PCM sample to the silence search.  This is the shared
   heart of the detector -- it is codec-independent. */
static void
findsep_process_sample (DECODE_STRUCT* ds, short sample)
{
    /* Instantaneous volume: RMS of this sample and the previous one. */
    double v = (double) ds->prev_sample * ds->prev_sample
	     + (double) sample * sample;
    v = sqrt (v / 2);
    if (ds->pcmpos > ds->len_to_sw_start_samp
	&& ds->pcmpos < ds->len_to_sw_end_samp) {
	search_for_silence (ds, v);
    }
    ds->pcmpos++;
    ds->prev_sample = sample;
}

/* After all PCM has been fed, pick the silence point and convert it to byte
   offsets (pos1/pos2) in the input buffer.  Frees the frame list. */
static void
findsep_finalize (DECODE_STRUCT* ds, long padding1, long padding2,
		  u_long* pos1, u_long* pos2)
{
    unsigned long silstart;
    int i;

    debug_printf ("total length:    %d\n", ds->pcmpos);
    debug_printf ("silence_samples: %d\n", ds->silence_samples);

    /* If nothing decoded, fall back to the middle of the buffer. */
    if (ds->frame_list.next == &ds->frame_list) {
	*pos1 = ds->mpgsize / 2;
	*pos2 = ds->mpgsize / 2;
	return;
    }

    /* Search through siltrackers to find minimum volume point */
    silstart = ds->pcmpos/2;
    for (i = 0; i < NUM_SILTRACKERS; i++) {
	if (ds->siltrackers[i].foundsil) {
	    silstart = ds->siltrackers[i].silstart_samp;
	    break;
	}
    }
    if (i == NUM_SILTRACKERS) {
	debug_printf ("warning: no silence found between tracks\n");
    }

    /* Now that we have the start of the silence, let's add the padding */
    apply_padding (ds, silstart, padding1, padding2, pos1, pos2);

    /* Free the list of frame info */
    free_frame_list (ds);
}

error_code
findsep_silence (const char* mpgbuf,
		 long mpgsize,
		 long len_to_sw,
		 long searchwindow,
		 long silence_length,
		 long padding1,
		 long padding2,
		 u_long* pos1,
		 u_long* pos2
		 )
{
    DECODE_STRUCT ds;
    mp3dec_t mp3d;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pos = 0;

    findsep_init_ds (&ds, mpgbuf, mpgsize, len_to_sw, searchwindow,
		     silence_length);

    debug_printf ("FINDSEP 1: %p -> %p (0x%x)\n",
	mpgbuf, mpgbuf+mpgsize, mpgsize);

    /* Decode the mp3 to PCM frame by frame (minimp3) and feed the silence
       search.  Each frame's byte offset -> pcm position is recorded (frame
       list) so the chosen silence sample can be mapped back to a cut byte
       offset in apply_padding(). */
    mp3dec_init (&mp3d);
    while (pos < mpgsize) {
	mp3dec_frame_info_t info;
	int samples = mp3dec_decode_frame (&mp3d,
	    (const uint8_t*) mpgbuf + pos, (int)(mpgsize - pos), pcm, &info);
	if (info.frame_bytes == 0)
	    break;   /* no more syncable mp3 data */
	if (samples > 0) {
	    long framepos = pos + info.frame_offset;
	    int ch = info.channels ? info.channels : 1;
	    int i;
	    FRAME_LIST* fl;
	    findsep_set_samplerate (&ds, info.hz);
	    fl = (FRAME_LIST*) malloc (sizeof(FRAME_LIST));
	    fl->m_framepos = ds.mpgbuf + framepos;
	    fl->m_samples = samples;
	    fl->m_pcmpos = ds.pcmpos;
	    list_add_tail (&(fl->m_list), &(ds.frame_list));
	    for (i = 0; i < samples; i++) {
		short s = (ch >= 2)
		    ? (short) (((int) pcm[i*ch] + (int) pcm[i*ch + 1]) / 2)
		    : pcm[i*ch];
		findsep_process_sample (&ds, s);
	    }
	}
	pos += info.frame_bytes;
    }

    findsep_finalize (&ds, padding1, padding2, pos1, pos2);
    return SR_SUCCESS;
}

static void 
init_siltrackers(SILENCETRACKER* siltrackers)
{
    int i;
    long stepsize = (MAX_RMS_SILENCE - MIN_RMS_SILENCE) / (NUM_SILTRACKERS-1);
    long rms = MIN_RMS_SILENCE;
    for (i = 0; i < NUM_SILTRACKERS; i++, rms += stepsize) {
	siltrackers[i].foundsil = 0;
	siltrackers[i].silstart_samp = 0;
	siltrackers[i].insilencecount = 0;
	siltrackers[i].silencevol = rms;
    }
}

static void
apply_padding (DECODE_STRUCT* ds,
	       unsigned long silstart,
	       long padding1,
	       long padding2,
	       u_long* pos1, 
	       u_long* pos2
	       )
{
    /* Compute positions in samples */
    FRAME_LIST *pos;
    long pos1s, pos2s;

    pos1s = silstart 
	    + (ds->silence_samples/2) 
	    + padding1 * (ds->samplerate/1000);
    pos2s = silstart 
	    + (ds->silence_samples/2) 
	    - padding2 * (ds->samplerate/1000);

    debug_printf ("Applying padding: p1,p2 = (%d,%d), pos1s,pos2s = (%d,%d)\n", padding1, padding2, pos1s, pos2s);

    /* GCS FIX: Need to check for pos == null */
    /* GCS FIX: Watch out for -1, might have mem error! */
    pos = list_entry (ds->frame_list.next, FRAME_LIST, m_list);
    if (pos1s < pos->m_pcmpos) {
	*pos1 = pos->m_framepos - ds->mpgbuf - 1;
    }
    if (pos2s < pos->m_pcmpos) {
	*pos2 = pos->m_framepos - ds->mpgbuf;
    }
    list_for_each_entry (pos, FRAME_LIST, &(ds->frame_list), m_list) {
	if (pos1s >= pos->m_pcmpos) {
	    *pos1 = pos->m_framepos - ds->mpgbuf - 1;
	}
	if (pos2s >= pos->m_pcmpos) {
	    *pos2 = pos->m_framepos - ds->mpgbuf;
	}
    }
    debug_printf ("pos1, pos2 = %d,%d (%d) (%02x%02x)\n",
		  *pos1, *pos2, 
		  *pos1 - *pos2, 
		  ds->mpgbuf[*pos2], 
		  ds->mpgbuf[*pos2+1]);
}

static void 
free_frame_list (DECODE_STRUCT* ds)
{
    FRAME_LIST *pos, *n;
    /* GCS: This seems to be the best way to go through a list.
       Note no compiler warnings. */
    list_for_each_entry_safe (pos, FRAME_LIST, n, &(ds->frame_list), m_list) {
	list_del (&(pos->m_list));
	free (pos);
    }
}

static void
search_for_silence (DECODE_STRUCT *ds, double vol)
{
    int i;
    for(i = 0; i < NUM_SILTRACKERS; i++) {
	SILENCETRACKER *pstracker = &ds->siltrackers[i];

	if (pstracker->foundsil)
	    continue;

	if (vol < pstracker->silencevol) {
	    if (pstracker->insilencecount == 0) {
		pstracker->silstart_samp = ds->pcmpos;
	    }
	    pstracker->insilencecount++;
	} else {
	    pstracker->insilencecount = 0;
	}

	if (pstracker->insilencecount > ds->silence_samples) {
	    pstracker->foundsil = TRUE;
	}
    }
}

/* Decode mp3 frames until one reports a bitrate.  Returns the bitrate in
   bits/sec (the caller divides by 1000 for kbps), or 0 if none found. */
error_code
find_bitrate (unsigned long* bitrate, const char* mpgbuf, long mpgsize)
{
    mp3dec_t mp3d;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pos = 0;

    *bitrate = 0;
    mp3dec_init (&mp3d);
    while (pos < mpgsize) {
	mp3dec_frame_info_t info;
	(void) mp3dec_decode_frame (&mp3d, (const uint8_t*) mpgbuf + pos,
				    (int)(mpgsize - pos), pcm, &info);
	if (info.frame_bytes == 0)
	    break;
	if (info.bitrate_kbps > 0) {
	    *bitrate = (unsigned long) info.bitrate_kbps * 1000;  /* -> bps */
	    debug_printf ("Decoded bitrate from stream: %ld\n", *bitrate);
	    break;
	}
	pos += info.frame_bytes;
    }
    return SR_SUCCESS;
}

#if defined (HAVE_FAAD)
/*****************************************************************************
 * AAC silence detection (faad2)
 *
 * The mp3 path above is driven by libmad's callbacks; here we drive faad2
 * ourselves, decoding one ADTS frame at a time to 16-bit PCM and feeding the
 * same silence search.  Each ADTS frame is recorded in the frame list (byte
 * position -> pcm position) exactly like the mp3 filter/output callbacks do,
 * so the resulting cut lands on an ADTS frame boundary.
 *****************************************************************************/

/* Find the next ADTS syncword (12 bits set: 0xFFF) at or after `start`.
   Returns the offset, or -1 if none. */
static long
adts_find_sync (const unsigned char* buf, long size, long start)
{
    long i;
    for (i = start; i + 1 < size; i++) {
	if (buf[i] == 0xFF && (buf[i+1] & 0xF6) == 0xF0)
	    return i;
    }
    return -1;
}

error_code
findsep_silence_aac (const char* aacbuf,
		     long aacsize,
		     long len_to_sw,
		     long searchwindow,
		     long silence_length,
		     long padding1,
		     long padding2,
		     u_long* pos1,
		     u_long* pos2
		     )
{
    DECODE_STRUCT ds;
    NeAACDecHandle dec;
    NeAACDecConfigurationPtr conf;
    unsigned long samplerate = 0;
    unsigned char channels = 0;
    long pos, synced, init_rc;

    /* Sensible fallback in case we bail early. */
    *pos1 = aacsize / 2;
    *pos2 = aacsize / 2;

    findsep_init_ds (&ds, aacbuf, aacsize, len_to_sw, searchwindow,
		     silence_length);

    debug_printf ("FINDSEP AAC: %p -> %p (0x%x)\n",
	aacbuf, aacbuf + aacsize, aacsize);

    dec = NeAACDecOpen ();
    if (!dec)
	return SR_SUCCESS;
    conf = NeAACDecGetCurrentConfiguration (dec);
    conf->outputFormat = FAAD_FMT_16BIT;
    NeAACDecSetConfiguration (dec, conf);

    /* Sync to the first ADTS frame and initialize the decoder there. */
    synced = adts_find_sync ((const unsigned char*) aacbuf, aacsize, 0);
    if (synced < 0) {
	NeAACDecClose (dec);
	return SR_SUCCESS;
    }
    init_rc = NeAACDecInit (dec, (unsigned char*) aacbuf + synced,
			    aacsize - synced, &samplerate, &channels);
    if (init_rc < 0) {
	NeAACDecClose (dec);
	return SR_SUCCESS;
    }
    pos = synced + init_rc;   /* usually 0 for ADTS -> starts at the sync */

    /* Decode frame by frame. */
    while (pos < aacsize) {
	NeAACDecFrameInfo fi;
	void* out;
	long framepos = pos;

	out = NeAACDecDecode (dec, &fi, (unsigned char*) aacbuf + pos,
			      aacsize - pos);
	if (fi.error != 0 || fi.bytesconsumed == 0) {
	    /* Resync to the next ADTS frame, if any. */
	    long nxt = adts_find_sync ((const unsigned char*) aacbuf, aacsize,
				       pos + 1);
	    if (nxt < 0)
		break;
	    pos = nxt;
	    continue;
	}

	/* Output sample rate is known only after decoding (HE-AAC/SBR doubles
	   it), so latch it from the frame, not from NeAACDecInit. */
	findsep_set_samplerate (&ds, (long) fi.samplerate);

	/* Record this frame's byte -> pcm mapping. */
	{
	    FRAME_LIST* fl = (FRAME_LIST*) malloc (sizeof(FRAME_LIST));
	    unsigned int nch = fi.channels ? fi.channels : 1;
	    fl->m_framepos = ds.mpgbuf + framepos;
	    fl->m_samples = fi.samples / nch;
	    fl->m_pcmpos = ds.pcmpos;
	    list_add_tail (&(fl->m_list), &(ds.frame_list));
	}

	/* Mono-mix the interleaved 16-bit PCM and feed the silence search. */
	if (out && fi.samples > 0) {
	    const short* pcm = (const short*) out;
	    unsigned int nch = fi.channels ? fi.channels : 1;
	    unsigned long nframes = fi.samples / nch;
	    unsigned long i;
	    for (i = 0; i < nframes; i++) {
		long s;
		if (nch >= 2)
		    s = ((long) pcm[i*nch] + (long) pcm[i*nch + 1]) / 2;
		else
		    s = pcm[i*nch];
		findsep_process_sample (&ds, (short) s);
	    }
	}

	pos += fi.bytesconsumed;
    }

    NeAACDecClose (dec);
    findsep_finalize (&ds, padding1, padding2, pos1, pos2);
    return SR_SUCCESS;
}
#endif /* HAVE_FAAD */
