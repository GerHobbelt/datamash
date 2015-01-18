/* GNU Datamash - perform simple calculation on input data

   Copyright (C) 2013-2015 Assaf Gordon <assafgordon@gmail.com>

   This file is part of GNU Datamash.

   GNU Datamash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU Datamash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU Datamash.  If not, see <http://www.gnu.org/licenses/>.
*/
/* Written by Assaf Gordon */
#ifndef __STRCNT_H__
#define __STRCNT_H__

#include <config.h>
#include <unistd.h>

/*
TODO:
See gnulib's strchrnul.c for possible optimizations.
*/

/* returns the number of times character C appears in
   NUL-terminated string str. */
size_t _GL_ATTRIBUTE_PURE
strcnt (const char* str, int c)
{
  size_t cnt = 0;
  while ( *str != 0 )
    {
      if ( *str == c )
        ++cnt;
      ++str;
    }
  return cnt;
}

#endif
