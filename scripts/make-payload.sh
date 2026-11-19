#!/bin/sh
# Assemble an install payload from install/* and the libraries you built (./build.sh):
#   sh scripts/make-payload.sh [OUT_DIR]   -> OUT_DIR/payload (default out/payload)
# Copy the payload directory to the Kindle (e.g. /mnt/us/kppfast-payload) and run `sh install.sh` there as root.
# Only libraries named in LIBS are included (default: the first, low-risk set; add the others one at a time).
set -e
R=$(cd "$(dirname "$0")/.." && pwd); O=${1:-${OUT:-$R/out}}; P=$O/payload
LIBS=${LIBS:-"libkppfs.so libkppx11.so"}
rm -rf "$P"; mkdir -p "$P/lib"
for f in launch.sh kppfast-lib.sh kppfast-ctl.sh install.sh rollback.sh check.sh env.default; do cp "$R/install/$f" "$P/$f"; done
for so in $LIBS; do [ -f "$O/$so" ] || { echo "missing $O/$so (run ./build.sh)" >&2; exit 1; }; cp "$O/$so" "$P/lib/"; done
sum() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
(cd "$P/lib" && sum *.so > SHA256SUMS)
(cd "$P" && sum launch.sh kppfast-lib.sh kppfast-ctl.sh install.sh rollback.sh check.sh env.default lib/*.so lib/SHA256SUMS > SHA256SUMS)
echo "payload $P: $(cd "$P" && ls lib/*.so | tr '\n' ' ')"
