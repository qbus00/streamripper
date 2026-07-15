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

/* Streaming mp3 -> 16-bit PCM WAV encoder.  Feed mp3 bytes as they arrive
   with wav_encoder_write(); the RIFF/WAVE header is written up front as a
   placeholder and patched with the final sizes/format by wav_encoder_close().
   The handle is stored as a void* (opaque) on the Writer. */
error_code wav_encoder_open  (void **handle, FHANDLE fp);
error_code wav_encoder_write (void *handle, const char *mp3, unsigned long size);
error_code wav_encoder_close (void *handle, FHANDLE fp);

#endif /* __WAV_OUTPUT_H__ */
