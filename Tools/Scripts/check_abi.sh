#!/bin/sh
# ABI check: the library must export exactly the dx_* symbols listed in Tests/Abi/symbols.txt.
# A removed symbol breaks existing users; an added one is an undeclared API change.
# Usage: check_abi.sh <path-to-libduplex>
# After a deliberate API change, regenerate the list with the same pipeline, e.g.:
#   check_abi.sh <lib> --print > Tests/Abi/symbols.txt   (and bump DX_ABI_VERSION)
baseline="$(dirname "$0")/../../Tests/Abi/symbols.txt"

# List exported symbols, keep the last column (the name), drop macOS's leading "_",
# keep only our own dx_* namespace, sort so the comparison is stable.
actual=$(nm -g --defined-only "$1" | awk '{print $NF}' | sed 's/^_//' | grep '^dx_' | sort)

[ "$2" = "--print" ] && { echo "$actual"; exit 0; }

# diff prints the added/removed symbols and exits non-zero on any difference.
echo "$actual" | diff - "$baseline"
