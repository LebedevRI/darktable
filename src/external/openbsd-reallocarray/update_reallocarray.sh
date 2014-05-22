#!/bin/bash

export CVSROOT='anoncvs@anoncvs.fr.openbsd.org:/cvs'

cvs checkout src/lib/libc/stdlib/reallocarray.c

mv src/lib/libc/stdlib/reallocarray.c ./

rm -rf src/

git diff --unified=15
