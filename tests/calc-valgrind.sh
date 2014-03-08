#!/bin/sh

#   Unit Tests for calc - perform simple calculation on input data
#   Copyright (C) 2014 Assaf Gordon.
# 
#    This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
# 
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
# 
#    You should have received a copy of the GNU General Public License
#    along with this program.  If not, see <http://www.gnu.org/licenses/>.
#    Written by Assaf Gordon

. "${test_dir=.}/init.sh"; path_prepend_ ./src

## Ensure valgrind is useable
## (copied from coreutils' init.cfg)
valgrind --error-exitcode=1 true 2>/dev/null ||
    skip_ "requires a working valgrind"

fail=0

seq 10000 | valgrind --leak-check=full --error-exitcode=1 \
                 calc unique 1 > /dev/null || { warn_ "unique 1 - failed" ; fail=1 ; }

seq 10000 | sed 's/^/group /' |
     valgrind --leak-check=full --error-exitcode=1 \
                 calc -g 1 unique 1 > /dev/null || { warn_ "-g 1 unique 1 - failed" ; fail=1 ; }

seq 10000 | valgrind --leak-check=full --error-exitcode=1 \
                 calc countunique 1 > /dev/null || { warn_ "countunique 1 - failed" ; fail=1 ; }

seq 10000 | valgrind --leak-check=full --error-exitcode=1 \
                 calc collapse 1 > /dev/null || { warn_ "collapse 1 - failed" ; fail=1 ; }

Exit $fail

