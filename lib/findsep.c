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
#include "mad.h"
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
#define READSIZE	2000
// #define READSIZE	1000

/* Uncomment to dump an mp3 of the search window. */
#define MAKE_DUMP_MP3 1

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
    long  mpgpos_next;      /* Position for next write to decoder */
    long  mpgpos_curr;      /* Position for current chunk being decoded */
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

typedef struct GET_BITRATE_STRUCTst
{
    unsigned long bitrate;
    unsigned char* mpgbuf;
    long mpgsize;
} GET_BITRATE_STRUCT;

/*****************************************************************************
 * Public functions
 *****************************************************************************/

/*****************************************************************************
 * Private functions
 *****************************************************************************/
static void init_siltrackers(SILENCETRACKER* siltrackers);
static void apply_padding (DECODE_STRUCT* ds, unsigned long silstart,
			   long padding1, long padding2,
			   u_long* pos1, u_long* pos2);
static void free_frame_list (DECODE_STRUCT* ds);
static enum mad_flow input(void *data, struct mad_stream *ms);
static void search_for_silence(DECODE_STRUCT *ds, double vol);
static signed int scale(mad_fixed_t sample);
static enum mad_flow output(void *data, struct mad_header const *header,
			    struct mad_pcm *pcm);
static enum mad_flow filter (void *data, struct mad_stream const *ms,
			     struct mad_frame *frame);
static enum mad_flow error(void *data, struct mad_stream *ms, 
			   struct mad_frame *frame);
static enum mad_flow header(void *data, struct mad_header const *pheader);
static enum mad_flow input_get_bitrate (void *data, struct mad_stream *stream);
static enum mad_flow header_get_bitrate (void *data, 
					 struct mad_header const *pheader);

/*****************************************************************************
 * Private Vars
 *****************************************************************************/

/*****************************************************************************
 * Functions
 *****************************************************************************/
static char* 
mad_error_string (enum mad_error mad_err)
{
    switch (mad_err) {
    case MAD_ERROR_NONE:
	return "MAD_ERROR_NONE";
    case MAD_ERROR_BUFLEN:
	return "MAD_ERROR_BUFPTR";
    case MAD_ERROR_NOMEM:
	return "MAD_ERROR_NOMEM";
    case MAD_ERROR_LOSTSYNC:
	return "MAD_ERROR_LOSTSYNC";
    case MAD_ERROR_BADLAYER:
	return "MAD_ERROR_BADLAYER";
    case MAD_ERROR_BADBITRATE:
	return "MAD_ERROR_BADBITRAT";
    case MAD_ERROR_BADSAMPLERATE:
	return "MAD_ERROR_BADSAMPLERATE";
    case MAD_ERROR_BADEMPHASIS:
	return "MAD_ERROR_BADEMPHASIS";
    case MAD_ERROR_BADCRC:
	return "MAD_ERROR_BADCRC";
    case MAD_ERROR_BADBITALLOC:
	return "MAD_ERROR_BADBITALLOC";
    case MAD_ERROR_BADSCALEFACTOR:
	return "MAD_ERROR_BADSCALEFACTOR";
#if defined (MAD_ERROR_BADMODE)
    case MAD_ERROR_BADMODE:
	return "MAD_ERROR_BADMODE";
#endif
    case MAD_ERROR_BADFRAMELEN:
	return "MAD_ERROR_BADFRAMELEN";
    case MAD_ERROR_BADBIGVALUES:
	return "MAD_ERROR_BADBIGVALUES";
    case MAD_ERROR_BADBLOCKTYPE:
	return "MAD_ERROR_BADBLOCKTYPE";
    case MAD_ERROR_BADSCFSI:
	return "MAD_ERROR_BADSCFSI";
    case MAD_ERROR_BADDATAPTR:
	return "MAD_ERROR_BADDATAPTR";
    case MAD_ERROR_BADPART3LEN:
	return "MAD_ERROR_BADPART3LEN";
    case MAD_ERROR_BADHUFFTABLE:
	return "MAD_ERROR_BADHUFFTABLE";
    case MAD_ERROR_BADHUFFDATA:
	return "MAD_ERROR_BADHUFFDATA";
    case MAD_ERROR_BADSTEREO:
	return "MAD_ERROR_BADSTEREO";
    default:
	return "(Unknown libmad error)";
    }
}

/* Initialize the shared decode/search state.  Codec-neutral: the mp3 (libmad)
   and aac (faad2) front-ends both call this before feeding PCM. */
static void
findsep_init_ds (DECODE_STRUCT* ds, const char* buf, long size,
		 long len_to_sw, long searchwindow, long silence_length)
{
    ds->mpgbuf = (unsigned char*) buf;
    ds->mpgsize = size;
    ds->pcmpos = 0;
    ds->mpgpos_curr = -1;
    ds->mpgpos_next = 0;
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
    struct mad_decoder decoder;

    findsep_init_ds (&ds, mpgbuf, mpgsize, len_to_sw, searchwindow,
		     silence_length);

    debug_printf ("FINDSEP 1: %p -> %p (0x%x)\n",
	mpgbuf, mpgbuf+mpgsize, mpgsize);

    /* Run decoder */
    mad_decoder_init (&decoder, &ds, input, header, filter, output,
	error, NULL);
    (void) mad_decoder_run (&decoder, MAD_DECODER_MODE_SYNC);
    mad_decoder_finish (&decoder);

    assert (ds.mpgsize != 0);
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

static enum mad_flow
input (void *data, struct mad_stream *ms)
{
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    long frameoffset = 0;
    long espnextpos = ds->mpgpos_next + READSIZE;

    /* GCS FIX: This trims the last READSIZE from consideration */
    if (espnextpos > ds->mpgsize) {
#if defined (commentout)
	debug_printf ("INP:  espnextpos=0x%x ds->mpgsize=0x%x\n",
	    espnextpos, ds->mpgsize);
#endif
	return MAD_FLOW_STOP;
    }

    if (ms->next_frame) {
	frameoffset = &(ds->mpgbuf[ds->mpgpos_next]) - ms->next_frame;
        /* GCS July 8, 2004
	   This is the famous frameoffset != READSIZE bug.
	   What appears to be happening is libmad is not syncing 
	   properly on the broken initial frame.  Therefore, 
	   if there is no header yet (hence no ds->samplerate),
	   we'll nudge along the buffer to try to resync.
	*/
	if (frameoffset == READSIZE) {
	    if (!ds->samplerate) {
		frameoffset--;
	    } else {
		FILE* fp;
		debug_printf (
		    "%p | %p | %p | %p | %d\n",
		    ds->mpgbuf, 
		    ds->mpgpos_next, 
		    &(ds->mpgbuf[ds->mpgpos_next]), 
		    ms->next_frame, 
		    frameoffset);
    		fprintf (stderr, "ERROR: frameoffset != READSIZE\n");
		debug_printf ("ERROR: frameoffset != READSIZE\n");
		fp = fopen ("gcs1.txt","w");
		fwrite(ds->mpgbuf,1,ds->mpgsize,fp);
		fclose(fp);
		exit (-1);
	    }
	}
    }
#if defined (commentout)
    debug_printf ("INP:  %p | 0x%x | %p | %p | 0x%x\n", 
	ms->buffer, 
	ms->bufend - ms->buffer, 
	ms->this_frame, 
	ms->next_frame,
	ms->skiplen
    );
#endif

    ds->mpgpos_curr = ds->mpgpos_next-frameoffset;
    mad_stream_buffer (ms, (const unsigned char*) 
	&ds->mpgbuf[ds->mpgpos_curr], READSIZE);
    ds->mpgpos_next = ds->mpgpos_curr + READSIZE;

#if defined (commentout)
    debug_printf (
	"INP:  %p | 0x%x | 0x%x | 0x%x | 0x%x | 0x%x\n",
	ds->mpgbuf, 
	ds->mpgpos_curr, 
	ds->mpgpos_next, 
	READSIZE, 
	frameoffset,
	READSIZE - frameoffset
    );
#endif

    return MAD_FLOW_CONTINUE;
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

static signed int
scale (mad_fixed_t sample)
{
    /* round */
    sample += (1L << (MAD_F_FRACBITS - 16));

    /* clip */
    if (sample >= MAD_F_ONE)
	sample = MAD_F_ONE - 1;
    else if (sample < -MAD_F_ONE)
	sample = -MAD_F_ONE;

    /* quantize */
    return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static enum mad_flow 
filter (void *data, struct mad_stream const *ms, struct mad_frame *frame)
{
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    FRAME_LIST* fl;

    fl = (FRAME_LIST*) malloc (sizeof(FRAME_LIST));
    fl->m_framepos = ms->this_frame;
    fl->m_samples = 0;
    fl->m_pcmpos = 0;
    list_add_tail (&(fl->m_list), &(ds->frame_list));

#if defined (commentout)
    debug_printf ("FLT:  %p | 0x%x | %p | %p | 0x%x\n", 
	ms->buffer, 
	ms->bufend - ms->buffer, 
	ms->this_frame, 
	ms->next_frame,
	ms->skiplen
    );
    debug_printf (
	"FLT:  (%02x%02x) | 0x%x | 0x%x | 0x%x | 0x%x\n", 
	ms->this_frame[0], ms->this_frame[1], 
	ds->mpgpos_curr,
	ms->this_frame - ms->buffer,
	ds->mpgpos_curr + (ms->this_frame - ms->buffer),
	ds->mpgpos_next
    );
#endif

    return MAD_FLOW_CONTINUE;
}

static enum mad_flow
output (void *data, struct mad_header const *header,
	struct mad_pcm *pcm)
{
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    FRAME_LIST *fl;
    unsigned int nchannels, nsamples;
    mad_fixed_t const *left_ch, *right_ch;
    signed int sample;

    nchannels = pcm->channels;
    nsamples  = pcm->length;
    left_ch   = pcm->samples[0];
    right_ch  = pcm->samples[1];

    /* Get frame entry */
    fl = list_entry (ds->frame_list.prev, FRAME_LIST, m_list);
    fl->m_samples = nsamples;
    fl->m_pcmpos = ds->pcmpos;

#if defined (commentout)
    if (ds->pcmpos > ds->len_to_sw_start_samp
	&& ds->pcmpos < ds->len_to_sw_end_samp) {
	debug_printf ("DEC *:  pcmpos 0x%x, nsamp 0x%x\n", 
	    fl->m_pcmpos, fl->m_samples);
    } else {
	debug_printf ("DEC -:  pcmpos 0x%x, nsamp 0x%x\n", 
	    fl->m_pcmpos, fl->m_samples);
    }
#endif

    while (nsamples--) {
	/* output sample(s) in 16-bit signed little-endian PCM */
	/* GCS FIX: Does this work on big endian machines??? */
	sample = (short) scale (*left_ch++);

	if (nchannels == 2) {
	    // make mono
	    sample = (sample+scale(*right_ch++))/2;
	}

	findsep_process_sample (ds, (short) sample);
    }

    return MAD_FLOW_CONTINUE;
}

static enum mad_flow 
header (void *data, struct mad_header const *pheader)
{
    DECODE_STRUCT *ds = (DECODE_STRUCT *)data;
    findsep_set_samplerate (ds, pheader->samplerate);
    return MAD_FLOW_CONTINUE;
}

static enum mad_flow
error (void *data, struct mad_stream *ms, struct mad_frame *frame)
{
    if (MAD_RECOVERABLE(ms->error)) {
	debug_printf ("mad error 0x%04x %s\n", ms->error, 
	    mad_error_string (ms->error));
	return MAD_FLOW_CONTINUE;
    }

    debug_printf ("unrecoverable mad error 0x%04x %s\n",
	mad_error_string (ms->error));
    return MAD_FLOW_BREAK;
}

/* The following routines have nothing to do with finding a separation 
 * point. Instead, they have to do with finding the bitrate.  However, 
 * they are included here because they are "mad" related.
 */
error_code
find_bitrate (unsigned long* bitrate, const char* mpgbuf, long mpgsize)
{
    struct mad_decoder decoder;
    GET_BITRATE_STRUCT gbs;
    
    /* initialize and start decoder */
    gbs.mpgbuf = (unsigned char*) mpgbuf;
    gbs.mpgsize = mpgsize;
    gbs.bitrate = 0;
    mad_decoder_init (
	&decoder,
	&gbs,
	input_get_bitrate /* input */,
	header_get_bitrate /* header */,
	NULL /* filter */,
	NULL /* output */,
	NULL /* error */,
	NULL /* message */);
    (void) mad_decoder_run (&decoder, MAD_DECODER_MODE_SYNC);
    mad_decoder_finish (&decoder);
    *bitrate = gbs.bitrate;
    return SR_SUCCESS;
}

static enum mad_flow
input_get_bitrate (void *data, struct mad_stream *stream)
{
    GET_BITRATE_STRUCT* gbs = (GET_BITRATE_STRUCT*) data;

    if (!gbs->mpgsize)
	return MAD_FLOW_STOP;

    mad_stream_buffer(stream, gbs->mpgbuf, gbs->mpgsize);
    gbs->mpgsize = 0;
    return MAD_FLOW_CONTINUE;
}

static enum mad_flow
header_get_bitrate (void *data, struct mad_header const *pheader)
{
    GET_BITRATE_STRUCT* gbs = (GET_BITRATE_STRUCT*) data;

    gbs->bitrate = pheader->bitrate;	/* stream bitrate (bps) */
    debug_printf ("Decoded bitrate from stream: %ld\n", gbs->bitrate);
    return MAD_FLOW_STOP;
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
