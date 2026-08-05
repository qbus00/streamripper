/* wav_output.h -- decode mp3 track data to a PCM .wav file on the fly.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef __WAV_OUTPUT_H__
#define __WAV_OUTPUT_H__

#include "srtypes.h"
#include "sr_compat.h"

/* Streaming <codec> -> 16-bit PCM WAV encoder.  content_type selects the
   decoder: CONTENT_TYPE_MP3 (minimp3, always), CONTENT_TYPE_AAC (faad2, when
   built with HAVE_FAAD), or CONTENT_TYPE_OGG (libvorbis, when OGG_VORBIS_FOUND).
   Feed encoded bytes as they arrive with wav_encoder_write(); the RIFF/WAVE
   header is written up front as a placeholder and patched with the final
   sizes/format.  The handle is stored as a void* (opaque) on the Writer.
   Returns SR_ERROR_* if the codec isn't supported in this build. */
error_code wav_encoder_open  (void **handle, FHANDLE fp, int content_type);
error_code wav_encoder_write (void *handle, const char *buf, unsigned long size);
error_code wav_encoder_close (void *handle, FHANDLE fp);

#endif /* __WAV_OUTPUT_H__ */
