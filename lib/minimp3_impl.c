/* minimp3_impl.c -- the single translation unit that compiles the minimp3
 * MP3 decoder (lieff/minimp3, CC0 / public domain).  Every other file just
 * #includes "minimp3.h" for the declarations.  minimp3 replaced libmad (which
 * was abandoned since 2004 and carried unpatched CVEs); it decodes the same
 * MP3 frames to PCM for silence-based track splitting, bitrate detection, and
 * --wav output. */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
