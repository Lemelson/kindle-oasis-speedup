# How the results were reproduced

> **Disclaimer.** Educational and research documentation of the author's personal experiments on their own device.
> This page records how the author built, installed and measured the code, for transparency. It is not a
> recommendation to modify any device. Modifying a Kindle may violate Amazon's terms of use, may void the warranty and
> can make the device unusable ("brick" it). No support is provided. Anything you do with it is entirely at your own
> risk. The project does not circumvent DRM or access protected content and must not be used to do so. Respect
> Amazon's copyrights and trademarks. Not affiliated with or endorsed by Amazon.

### Responsible use

- Only on a device you own. Only Kindle Oasis 3 on firmware 5.18.2: everything else is refused by the checks or untested.
- Never redistribute Amazon files: not the device libraries you link against, not the generated `hot_bytes.h`,
  not a `libkpphot.so` you built (it embeds those firmware bytes), not symbol dumps, not screenshots of your books.
  `.gitignore` excludes them; keep it that way in forks. The other built libraries contain no firmware bytes, but
  sharing binaries is not needed: everyone builds from source.
- Do not use any of this to get at book content or to remove copy protection. Nothing here does that.
- Keep a way back before you start (kill switch, KUAL safe mode, `rollback.sh`), and test one switch at a time.

### 0. Prerequisites

- A Kindle Oasis 3, FW 5.18.2, that **you have already jailbroken yourself** with root shell access (SSH or USB
  network) and KUAL. This project does not provide, explain or link jailbreak tools.
- A computer with Docker (the build runs in an `linux/arm/v7` container under QEMU) and Python 3.
- `ssh` access configured under a host alias; the scripts use `KINDLE=<alias>` (default `kindle`).

### 1. Collect your own device files (private, never commit)

    mkdir -p ~/kindle-private/klibs ~/kindle-private/syms
    scp kindle:/lib/libc.so.6 kindle:/lib/libpthread.so.0 kindle:/lib/libdl.so.2 ~/kindle-private/klibs/

The libraries are linked against your device's glibc 2.20, so the result never needs a newer symbol version.
For `libkpphot` you also need the binaries it patches (names in `tools/genbytes.py`, e.g. `/app/bin/KPPMainAppV2`,
`libgdk-x11-2.0.so.0.2000.1`, `libcairo.so.2.11000.0`, `libX11.so.6.3.0`, `libImageUtils.so`, `libReactNative.so`,
`libpixman-1.so.0.18.4`; find them with `cat /proc/$(pidof KPPMainApp)/maps`). Copy them into `~/kindle-private/syms`.

### 2. Check that your firmware matches

    python3 -I tools/genbytes.py --hash ~/kindle-private/syms > my-hashes.txt
    diff my-hashes.txt docs/EXPECTED-HASHES.txt && echo "identical binaries"

This is the hash-only variant: it writes no firmware bytes. `EXPECTED-HASHES.txt` lists the stock 5.18.2 files. If
anything differs, stop: the offsets in this project do not apply to your binaries, and the runtime checks would refuse
every patch anyway. (On the author's device `KPPMainAppV2` itself had a different sha256 because of an unrelated
modification outside all checked regions; its region hashes were identical.)

### 3. Build

    python3 -I tools/genbytes.py ~/kindle-private/syms ~/kindle-private/hot_bytes.h   # stays outside the repo
    KLIBS=~/kindle-private/klibs HOTBYTES=~/kindle-private/hot_bytes.h ./build.sh test

`./build.sh` builds the `kppbuild` image from `docker/Dockerfile` (Debian bookworm armhf + gcc) on first use, builds
every `lib*.so` + `kinject` into `out/`, and `test` also runs the unit tests (grip, cover cache, I/O) that need no
device. Without `HOTBYTES` everything except `libkpphot.so` builds. Manual toolchain notes: any armv7 hard-float
gcc works if you pass `-march=armv7-a -mfpu=neon -mfloat-abi=hard -nostdlib` and link against the device's
`libc.so.6`/`libpthread.so.0` (see `scripts/build-in-container.sh` for the exact flags per library);
check with `readelf -V` that no `GLIBC_` version above 2.20 is referenced.

### 4. Install the launcher with the first libraries

    LIBS="libkppfs.so libkppx11.so" sh scripts/make-payload.sh
    scp -r out/payload kindle:/mnt/us/kppfast-payload
    ssh kindle 'sh /mnt/us/kppfast-payload/install.sh && sh /var/local/kppfast/check.sh'

`install.sh` refuses unless the firmware is 5.18.2 and `/etc/upstart/kppmainapp.conf` is exactly the stock job
(by sha256). It adds one line to that job and puts everything else into `/var/local/kppfast`. See `install/README.md`.

### 5. Enable switches one at a time

    KINDLE=kindle sh bench/device-addlib.sh out/libkppmalloc.so                # copy + register its sha256
    ssh kindle 'sh /var/local/kppfast/kppfast-ctl.sh all --no-restart'          # preload every library in lib/
    ssh kindle 'sh /var/local/kppfast/kppfast-ctl.sh set KPPMALLOC=1'           # set the switch and restart

`install/env.default` already sets the `libkppfs`/`libkppx11` switches. Order that worked here: fs + x11 → malloc +
img → hot (each `KPPHOT_*` separately) → side → db → log → cc (`KPPCC=1`, then `KPPCC_CAP_MB=16`, then
`KPPCC_BOOK_MB=16 KPPCC_BOOK=1`) → grip. `bench/device-baseline.sh` writes the full measured configuration to the
device (read it first; it expects all ten libraries and the bench helpers in `/var/local/kpp2`).

Two notes on that configuration: `KPPSIDE_WEBLAB=1` stops the app's A/B-experiment ("weblab") exposure records from
being passed on (see the README, "Amazon services and telemetry"); leave it off if you do not want to change what
the device reports. `KPPGRIP=1` was measured with `src/kppgrip/kppgrip.c` (v1); `kppgrip-v2-experimental.c` failed its first
device test (see RESULTS).

### 6. Measure (A/B)

    KINDLE=kindle sh bench/device-lock.sh acquire me
    B="all:PRE=libkppfs.so,libkppx11.so"; N="all:KPPMALLOC=1:PRE=libkppfs.so,libkppx11.so,libkppmalloc.so"
    MODES="$B $N $B $N" REPS=2 TURNS=20 sh bench/ab-run.sh
    python3 bench/compare.py runs/kpp-bench-*
    sh bench/device-lock.sh release me

The bench needs a Library with enough items for 20+ pages and injects key presses (`kinject`). A gain counts only if
it repeats in both rounds of the interleaved run. Details: [METHODOLOGY.md](METHODOLOGY.md).

### 7. Verify screens

Take screenshots (`/dev/fb0` → PNG) of the same pages with and without the candidate after a restart, mask the
status bar (rows 0–90), compare pixel by pixel. Keep these screenshots private: they show your library.

### 8. Roll back

- Instantly, without a computer: KUAL → "Safe mode" → accelerator off (install `safemode/extension` as a KUAL
  extension first).
- Over USB: create an empty file `DISABLE_KPPFAST` in the root of the Kindle drive and restart.
- Completely: `ssh kindle 'sh /var/local/kppfast/rollback.sh --restart'` (stock upstart job back, directory removed).

### Java / Hermes

The shipped speed-up does not patch Java or Hermes bytecode, and this repository contains no tool for doing so.
