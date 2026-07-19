/*
 * Copyright (C) 2001 Peter Harris <peter.harris@hummingbird.com>
 * Copyright (C) 2001 Edmund Grimley Evans <edmundo@rano.org>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Convert a string between UTF-8 and the locale's charset.
 */

#include <stdlib.h>
#include <string.h>

#include "utf8.h"
#include "charset.h"



#ifdef HAVE_LANGINFO_CODESET
#include <langinfo.h>
#endif

int iconvert(const char *fromcode, const char *tocode,
	     const char *from, size_t fromlen,
	     char **to, size_t *tolen);

#if defined (commentout)
static char *current_charset = 0; /* means "US-ASCII" */

void convert_set_charset(const char *charset)
{

  if (!charset)
    charset = getenv("CHARSET");

#ifdef HAVE_LANGINFO_CODESET
  if (!charset)
    charset = nl_langinfo(CODESET);
#endif

  free(current_charset);
  current_charset = 0;
  if (charset && *charset)
    current_charset = strdup(charset);
}
#endif

static int convert_buffer(const char *fromcode, const char *tocode,
			  const char *from, size_t fromlen,
			  char **to, size_t *tolen)
{
  int ret = -1;

#ifdef HAVE_ICONV
  ret = iconvert(fromcode, tocode, from, fromlen, to, tolen);
  if (ret != -1)
    return ret;
#endif

#ifndef HAVE_ICONV /* should be ifdef USE_CHARSET_CONVERT */
  ret = charset_convert(fromcode, tocode, from, fromlen, to, tolen);
  if (ret != -1)
    return ret;
#endif

  return ret;
}

static int convert_string(const char *fromcode, const char *tocode,
			  const char *from, char **to, char replace)
{
  int ret;
  size_t fromlen;
  char *s;

  fromlen = strlen(from);
  ret = convert_buffer(fromcode, tocode, from, fromlen, to, 0);
  if (ret == -2)
    return -1;
  if (ret != -1)
    return ret;

  s = malloc(fromlen + 1);
  if (!s)
    return -1;
  strcpy(s, from);
  *to = s;
  for (; *s; s++)
    if (*s & ~0x7f)
      *s = replace;
  return 3;
}

#if defined (commentout)
int utf8_encode(const char *from, char **to)
{
  char *charset;

  if (!current_charset)
    convert_set_charset(0);
  charset = current_charset ? current_charset : "US-ASCII";
  return convert_string(charset, "UTF-8", from, to, '#');
}
#endif

int utf8_decode(const char *from, char **to)
{
    if(*from == 0) {
	*to = malloc(1);
	**to = 0;
	return 1;
    }

#if defined (commentout)
    if (!current_charset)
	convert_set_charset(0);
    charset = current_charset ? current_charset : "US-ASCII";
    return convert_string("UTF-8", charset, from, to, '?');
#endif
    return convert_string("UTF-8", "US-ASCII", from, to, '?');

}

