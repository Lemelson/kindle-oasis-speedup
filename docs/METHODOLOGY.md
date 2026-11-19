# Methodology

## Rules
- Goal: make the Library as fast as possible while staying 1:1 with stock: identical screens, identical behaviour of
  every feature, nothing broken.
- Every change has its own `KPP*` switch, default off, with fallback to the original on anything unexpected, and is
  gated on the exact firmware code it depends on (file size + exact bytes or hash of every patched region).
- Never write Amazon's databases or configs (`/var/local/cc.db`), never modify `/usr`, `/opt/amazon`, `/app`; the only
  rootfs change is one line in `/etc/upstart/kppmainapp.conf`, done by `install/install.sh` with sha256 checks.
- Never put extra files into `/opt/amazon/ebook/lib` (the framework loads every file there).

## Finding the cost
1. Profile on the device with `src/kppprof` (SIGPROF sampler, `prof-run.sh`, `analyze.py`): CPU per thread, library
   and function per page turn. Stripped libraries mislabel symbols: check raw PC-offset histograms.
2. Read what the hot code really is (disassembly of your own device files, kept private), then decide: interpose an
   exported function, patch a few instructions in memory, or change a parameter of Amazon's own code.
3. Prefer un-starving Amazon's own mechanisms (cache size limits) over new code paths.

## Measuring
- Device first; an emulator of the same firmware (QEMU user mode) only for risky experiments: its timings are not
  representative.
- `bench/kpp-bench.sh` runs on the device: restart KPPMainApp, open the Library, N forward turns from page 1, N back,
  N forward again; one JSON line per turn (settle ms, main/JS ticks); a hash of the first cover row after every turn
  catches turns that did not change the page.
- `bench/ab-run.sh` interleaves modes (B N B N), unsets every key a mode does not set, and records each run's real
  environment (`/proc/<pid>/environ`) so a mislabelled run is detected. `bench/compare.py` prints medians.
- A gain counts only if it repeats in both rounds. Runs disturbed by handling the device are discarded, not fixed.
- One device, several sessions: `bench/device-lock.sh` (exclusive lock with a health gate, `device-health.sh`) around
  anything that restarts the app, benches or takes screenshots.

## Batch testing on the device
Candidates are prepared on the computer as `install.sh` / `rollback.sh` / `test.sh`. A batch session: lock + health
check → every candidate's test on the current baseline → install all → one restart → every test again → screenshot
parity on fixed pages (Library p1/p3, Home, a book page, quick menu; status bar rows 0–90 masked) → health check →
keep or roll back each candidate → report → new baseline if all clean.

## Parity
- Screens: baseline vs candidate on the same pages after a restart, pixel-exact outside the status bar.
- Caches: cover-cache files compared byte for byte (header bytes Amazon leaves uninitialised are masked).
- Behaviour: sort sheet, list view, filter sheet, Home ↔ Library, book open, quick settings.
- Health: no new crash reports, VmRSS sane, fail-safe counters clean.

## Traps worth knowing
- Each deliberate kill of KPPMainApp can produce Amazon's exit-crash report in the documents folder; it then shows
  up as the newest Library item and shifts every cover. Move it away before comparing screens.
- `/tmp` and `/var` are small RAM tmpfs; Amazon's crash dumper can fill `/var` for a minute.
- The device `ps` shows only command names: use pid files for liveness checks.
- If the reader is the active app, a KPPMainApp restart ends in "Page cannot be displayed": switch to Home first.
- A library constructor runs as root before the app drops privileges: never create the app's cache directories
  yourself (wrong owner), recreate them only from recorded uid/gid/mode.
