#!/bin/sh
# Host-side build of every kpp* library + test helpers for the Kindle Oasis 3 (armv7 hf, glibc 2.20).
#
#   KLIBS=/path/to/your/klibs ./build.sh [libs|test]          (default: libs)
#
# KLIBS (required): a private directory with libc.so.6, libpthread.so.0 and libdl.so.2 copied from YOUR OWN Kindle
#   (/lib on the device). They are only linked against; they never enter this repository (see .gitignore).
# HOTBYTES (optional): path of the hot_bytes.h you generated with tools/genbytes.py from your own device's
#   binaries. Without it libkpphot.so is skipped (every other library builds without firmware data).
# OUT (default ./out), KPP_IMAGE (default kppbuild).
set -e
R=$(cd "$(dirname "$0")" && pwd); OUT=${OUT:-$R/out}; IMG=${KPP_IMAGE:-kppbuild}; WHAT=${1:-libs}
: "${KLIBS:?set KLIBS to a directory with libc.so.6 libpthread.so.0 libdl.so.2 from your own Kindle}"
for f in libc.so.6 libpthread.so.0 libdl.so.2; do [ -f "$KLIBS/$f" ] || { echo "missing $KLIBS/$f" >&2; exit 1; }; done
docker image inspect "$IMG" >/dev/null 2>&1 || docker build --platform linux/arm/v7 -t "$IMG" "$R/docker"
mkdir -p "$OUT"; HV=""
if [ -n "${HOTBYTES:-}" ]; then HV="-v $(cd "$(dirname "$HOTBYTES")" && pwd)/$(basename "$HOTBYTES"):/hot/hot_bytes.h:ro"; fi
exec docker run --rm --platform linux/arm/v7 --network none --tmpfs /tmp:exec,size=300m --tmpfs /mnt:size=200m \
    --tmpfs /var/local:size=20m -v "$R":/src:ro -v "$(cd "$KLIBS" && pwd)":/klibs:ro -v "$OUT":/out $HV \
    "$IMG" sh /src/scripts/build-in-container.sh "$WHAT"
