#!/bin/bash
# collect-libs.sh - Recursively resolves all shared library dependencies
# for an ARM binary and copies them into a deploy directory.
#
# Usage: ./scripts/collect-libs.sh <elf-binary> <output-dir>
#
# Skips glibc (libc, ld-linux, libpthread, libm, libdl, librt, libresolv, libutil,
# libnsl, libanl) and the compiler runtime libgcc_s: they belong to the board's OS and
# must match its glibc; bundling older copies would break name resolution etc.

set -e

BINARY="$1"
OUTDIR="$2"
READELF="arm-linux-gnueabihf-readelf"
SEARCH_DIRS="/usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf"

# OS libraries - never bundle these
SKIP_PATTERN="^(libc\.so|ld-linux|libpthread|libm\.so|libdl\.so|librt\.so|libresolv\.so|libutil\.so|libnsl\.so|libanl\.so|libgcc_s\.so)"

declare -A SEEN  # tracks already-processed sonames

find_lib() {
    local soname="$1"
    for dir in $SEARCH_DIRS; do
        local path="$dir/$soname"
        if [ -e "$path" ]; then
            readlink -f "$path"
            return 0
        fi
    done
    return 1
}

collect() {
    local file="$1"
    # Get all NEEDED entries
    local needed
    needed=$($READELF -d "$file" 2>/dev/null | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/')

    for soname in $needed; do
        # Skip system libs
        if echo "$soname" | grep -qE "$SKIP_PATTERN"; then
            continue
        fi
        # Skip already processed
        if [ "${SEEN[$soname]+set}" = "set" ]; then
            continue
        fi
        SEEN[$soname]=1

        local realpath
        if realpath=$(find_lib "$soname"); then
            cp -fL "$realpath" "$OUTDIR/$soname"
            echo "  Collected $soname"
            # Recurse into this library's own dependencies
            collect "$realpath"
        else
            echo "  WARNING: $soname not found in search paths"
        fi
    done
}

mkdir -p "$OUTDIR"
echo "  Scanning $BINARY ..."
collect "$BINARY"
echo "  Total: ${#SEEN[@]} libraries collected"
