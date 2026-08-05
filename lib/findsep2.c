/* findsep2.c
 * library routines for finding silent points in mp3 data (the --xs2 variant)
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
 * MP3 decoding uses minimp3 (lieff/minimp3, public domain); the silence
 * search itself is unchanged from the original libmad-based implementation.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdint.h>
#include "minimp3.h"
#include "findsep.h"
#include "srtypes.h"
#include "debug.h"
#include "list.h"

#define MAX_RMS_SILENCE		32767 //max short

typedef struct FRAME_LIST_struct FRAME_LIST;
struct FRAME_LIST_struct
{
    const unsigned char* m_framepos;
    long m_samples;
    long m_pcmpos;
    LIST m_list;
};

typedef struct MIN_POSst
{
    unsigned short volume;
    unsigned long pos;
} MIN_POS;

typedef struct DECODE_STRUCTst
{
    unsigned char* mpgbuf;
    long  mpgsize;
    long len_to_sw_ms;
    long searchwindow_ms;
    long  silence_ms;
    long  silence_samples;
    unsigned long len_to_sw_start_samp;
    unsigned long len_to_sw_end_samp;
    unsigned long  pcmpos;
    long  samplerate;
    short prev_sample;
    LIST frame_list;
    unsigned short* maxvolume_buffer;
    unsigned long maxvolume_buffer_offs;
    int maxvolume_buffer_depth;
    int max_search_depth;
    MIN_POS* min_maxvolume_buffer;
} DECODE_STRUCT;

/*****************************************************************************
 * Private functions
 *****************************************************************************/
static void apply_padding (DECODE_STRUCT* ds, unsigned long silsplit,
			   long padding1, long padding2,
			   u_long* pos1, u_long* pos2);
static void free_frame_list (DECODE_STRUCT* ds);
static void search_for_silence(DECODE_STRUCT *ds, unsigned short vol);
static void init_maxvolume_buffer(DECODE_STRUCT *ds);

/*****************************************************************************
 * Functions
 *****************************************************************************/
error_code
findsep_silence_2 (const char* mpgbuf,
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
    int bestsil;
    int i;
    double delta = 1;

    ds.mpgbuf = (unsigned char*)mpgbuf;
    ds.mpgsize = mpgsize;
    ds.pcmpos = 0;
    ds.samplerate = 0;
    ds.prev_sample = 0;
    ds.len_to_sw_ms = len_to_sw;
    ds.searchwindow_ms = searchwindow;
    ds.silence_ms = silence_length;
    ds.maxvolume_buffer = 0;
    ds.maxvolume_buffer_offs = 0;
    ds.maxvolume_buffer_depth = 0;
    ds.max_search_depth = 0;
    ds.min_maxvolume_buffer = 0;
    INIT_LIST_HEAD (&ds.frame_list);

    debug_printf ("FINDSEP 2: %p -> %p (%d)\n", mpgbuf, mpgbuf+mpgsize, mpgsize);

    /* Decode the mp3 to PCM frame by frame (minimp3) and feed the silence
       search.  The maxvolume tree is initialized once the sample rate is known
       (first decoded frame). */
    mp3dec_init (&mp3d);
    while (pos < mpgsize) {
	mp3dec_frame_info_t info;
	int samples = mp3dec_decode_frame (&mp3d,
	    (const uint8_t*) mpgbuf + pos, (int)(mpgsize - pos), pcm, &info);
	if (info.frame_bytes == 0)
	    break;
	if (samples > 0) {
	    long framepos = pos + info.frame_offset;
	    int ch = info.channels ? info.channels : 1;
	    int j;
	    FRAME_LIST* fl;

	    if (!ds.samplerate) {
		ds.samplerate = info.hz;
		ds.len_to_sw_start_samp = ds.len_to_sw_ms * (ds.samplerate/1000);
		ds.len_to_sw_end_samp = (ds.len_to_sw_ms + ds.searchwindow_ms)
		    * (ds.samplerate/1000);
		init_maxvolume_buffer (&ds);   /* also sets silence_samples */
		debug_printf ("Setting samplerate: %ld\n", ds.samplerate);
	    }

	    fl = (FRAME_LIST*) malloc (sizeof(FRAME_LIST));
	    fl->m_framepos = ds.mpgbuf + framepos;
	    fl->m_samples = samples;
	    fl->m_pcmpos = ds.pcmpos;
	    list_add_tail (&(fl->m_list), &(ds.frame_list));

	    for (j = 0; j < samples; j++) {
		short s = (ch >= 2)
		    ? (short) (((int) pcm[j*ch] + (int) pcm[j*ch + 1]) / 2)
		    : pcm[j*ch];
		double v = (double) ds.prev_sample * ds.prev_sample
			 + (double) s * s;
		v = sqrt (v / 2);
		if (ds.pcmpos > ds.len_to_sw_start_samp
		    && ds.pcmpos < ds.len_to_sw_end_samp) {
		    search_for_silence (&ds, (unsigned short) v);
		}
		ds.pcmpos++;
		ds.prev_sample = s;
	    }
	}
	pos += info.frame_bytes;
    }

    debug_printf ("total length:    %d\n", ds.pcmpos);
    debug_printf ("silence_samples: %d\n", ds.silence_samples);

    /* If nothing decoded, fall back to the middle of the buffer. */
    if (ds.frame_list.next == &ds.frame_list) {
	*pos1 = *pos2 = ds.mpgsize / 2;
	free (ds.maxvolume_buffer);
	free (ds.min_maxvolume_buffer);
	return SR_SUCCESS;
    }

    /* Search through siltrackers to find minimum volume point.  Start with
       the highest silence-length. */
    bestsil = 0;
    for (i = 1; i <= ds.max_search_depth; ++i)
    {
        unsigned long current = ds.min_maxvolume_buffer[bestsil].volume;
        unsigned long candidate = ds.min_maxvolume_buffer[i].volume;

        delta *= 0.6;

        /* Only halve the silence-length if we can reduce the
           max-volume by at least 40 % by doing so. */
        if (current * delta > candidate)
        {
            bestsil = i;
            delta = 1;
        }
    }

    debug_printf("Most silent region: depth %d, max-volume %d, pos %ld,"
            "sample window %ld (%f ms)\n",
            bestsil,
            ds.min_maxvolume_buffer[bestsil].volume,
            ds.min_maxvolume_buffer[bestsil].pos,
            ds.silence_samples / (1 << bestsil),
            ds.silence_samples
            * 1000.0 / (double)(ds.samplerate * (1 << bestsil)));

    /* Now that we have the silence position, let's add the padding */
    apply_padding (&ds, ds.min_maxvolume_buffer[bestsil].pos,
            padding1, padding2, pos1, pos2);

    /* Free the max volume buffers */
    free (ds.maxvolume_buffer);
    free (ds.min_maxvolume_buffer);

    /* Free the list of frame info */
    free_frame_list (&ds);

    return SR_SUCCESS;
}

static void
apply_padding (DECODE_STRUCT* ds,
	       unsigned long silsplit,
	       long padding1,
	       long padding2,
	       u_long* pos1,
	       u_long* pos2
	       )
{
    /* Compute positions in samples */
    FRAME_LIST *pos;
    long pos1s, pos2s;

    pos1s = silsplit
	    + padding1 * (ds->samplerate/1000);
    pos2s = silsplit
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
propagate_max_value (unsigned short *max_buffer, unsigned long node_pos, int depth)
{
    unsigned long current_pos = node_pos;
    unsigned long parent_pos = current_pos / 2;

    while (depth-- > 0)
    {
        unsigned long sibling_pos = current_pos ^ 1;

        unsigned short current_val = max_buffer[current_pos];
        unsigned short sibling_val = max_buffer[sibling_pos];

        if (current_val > sibling_val)
            max_buffer[parent_pos] = current_val;
        else
            max_buffer[parent_pos] = sibling_val;

        current_pos = parent_pos;
        parent_pos /= 2;
    }
}

static void
insert_value (DECODE_STRUCT *ds, unsigned short vol, unsigned long pos)
{
    int i;
    unsigned long current_pos = 1;
    unsigned short prev = ds->maxvolume_buffer[ds->maxvolume_buffer_offs];
    int depth = 1;

    ds->maxvolume_buffer[ds->maxvolume_buffer_offs] = vol;
    vol = prev;

    for (i = 1; i < ds->maxvolume_buffer_depth; ++i)
    {
        unsigned long index = ds->maxvolume_buffer_offs + current_pos
            + (pos % current_pos);

        prev = ds->maxvolume_buffer[index];
        ds->maxvolume_buffer[index] = vol;
        vol = prev;

        propagate_max_value (ds->maxvolume_buffer, index, depth++);

        current_pos *= 2;
    }
}

static void
search_for_silence (DECODE_STRUCT *ds, unsigned short vol)
{
    unsigned long window_size = 1;
    unsigned long window_pos = ds->pcmpos - ds->len_to_sw_start_samp;
    unsigned long node_pos = ds->maxvolume_buffer_offs;
    int i;

    insert_value (ds, vol, ds->pcmpos);

    window_size = 1;
    for (i = ds->maxvolume_buffer_depth - 1; i >= 0; --i) {
        if (i <= ds->max_search_depth
            && window_pos >= window_size
            && ds->maxvolume_buffer[node_pos] < ds->min_maxvolume_buffer[i].volume)
        {
            ds->min_maxvolume_buffer[i].volume = ds->maxvolume_buffer[node_pos];
            ds->min_maxvolume_buffer[i].pos = ds->pcmpos - window_size / 2;
        }

        node_pos /= 2;
        window_size *= 2;
    }
}

static unsigned long
next_power_of_two(long value)
{
    long result = 1;
    while (result < value)
        result *= 2;
    return result;
}

static void
init_maxvolume_buffer(DECODE_STRUCT *ds)
{
    unsigned long buffer_offset = 1;
    unsigned long temp;
    unsigned long depth = 0;
    unsigned long i;

    ds->silence_samples =
        next_power_of_two(ds->silence_ms * (ds->samplerate/1000));

    temp = ds->silence_samples;
    while (temp > 0) {
        ++depth;
        temp /= 2;
        buffer_offset += temp;
    }

    ds->maxvolume_buffer_depth = depth;

    /* Let the minimum silence-length be 10 ms. */
    ds->max_search_depth =
        depth - (int)ceil(log(10 * (ds->samplerate / 1000.0)) / log(2)) - 1;

    /* Unless the user specified silence-length is lower. */
    if (ds->max_search_depth < 0)
        ds->max_search_depth = 0;

    ds->maxvolume_buffer =
        (unsigned short*) calloc(buffer_offset + ds->silence_samples,
                sizeof(unsigned short));
    ds->maxvolume_buffer_offs = buffer_offset;

    ds->min_maxvolume_buffer = (MIN_POS*) malloc(depth * sizeof(MIN_POS));

    for (i = 0; i < depth; ++i)
    {
        ds->min_maxvolume_buffer[i].volume = MAX_RMS_SILENCE;
        ds->min_maxvolume_buffer[i].pos = 0;
    }
}
