#!/bin/sh
# Source rules that a compiler cannot enforce (see DOCUMENTATION.md, "Memory"):
#  - the library allocates only in Source/Core/Ctx/context.c (default allocator and dx_create),
#    so nothing can allocate while polling, sending or decoding;
#  - the library never prints (logging, if ever added, goes through a callback).
# Exit code 0 = OK. Prints every offending line.
cd "$(dirname "$0")/../.." || exit 1

status=0

# Word-boundary match of the C allocation functions, outside the one allowed file.
found=$(grep -rnE '\b(malloc|calloc|realloc|free|strdup)[[:space:]]*\(' Source |
    grep -v '^Source/Core/Ctx/context.c')
if [ -n "$found" ]; then echo "allocation outside context.c:"; echo "$found"; status=1; fi

found=$(grep -rnE '\b(printf|fprintf|puts|perror)[[:space:]]*\(' Source)
if [ -n "$found" ]; then echo "printing inside the library:"; echo "$found"; status=1; fi

exit $status
