# kppfast: an unofficial UI speed-up for the Kindle Oasis 3

[Reproduce](docs/REPRODUCE.md) · [Results](docs/RESULTS.md) · [Methodology](docs/METHODOLOGY.md)

> **Disclaimer.** This is educational and research documentation of the author's personal experiments on their own
> device. It is not a recommendation to modify any device. Modifying a Kindle may violate Amazon's terms of use, may
> void the warranty and can make the device unusable ("brick" it). There is no support. Anything you do with it is
> entirely at your own risk. The project does not circumvent DRM, does not access protected content and must not be
> used to do so. It contains no Amazon firmware, binaries or code copied from them. Respect Amazon's copyrights and
> trademarks. This is an independent project, not affiliated with or endorsed by Amazon; Amazon, Kindle and Kindle
> Oasis are trademarks of Amazon.com, Inc. or its affiliates and are used only to describe the device this research
> concerns.

The Library of a Kindle Oasis 3 (10th generation, firmware 5.18.2) turns pages slowly, about 0.7 s per page. This
repository holds the author's own code that speeds up Amazon's stock `KPPMainApp` without replacing a single Amazon
file: small `libkpp*.so` libraries loaded with `LD_PRELOAD`, a fail-safe launcher, a benchmark harness and a KUAL
"Safe mode" extension.

![Library page turn timeline](docs/img/page-turn-timeline.svg)

## Results (one device, medians)

| | Amazon | kppfast |
|---|---|---|
| Library page turn, page seen before (also after a reboot) | 0.70–0.74 s | **≈0.26–0.28 s** |
| Library page turn, page never seen before (caches empty) | 0.70–0.74 s | **≈0.42 s** |
| Main-thread CPU per turn | 341 ms | 210 ms without the item cache; 60–70 ms on a revisit with it |
| Page turn while selecting text across pages | ≈1.2–1.25 s, then the finger lift is lost | ≈0.94–1.07 s, lift delivered |

Page-turn time = injected key press until KPPMainApp's main and JS threads are idle (the e-ink refresh itself is not
included; the time includes ~60–76 ms of key injection). CPU: SIGPROF profiles and per-turn tick counts. Details and
the per-step table: [RESULTS](docs/RESULTS.md).

Screens were compared with stock pixel by pixel. They match, except that the larger caches make Amazon's app show
its own cached variant of a cover tile more often: Amazon stores tiles with 16 grey levels, so a cached tile differs
from a freshly decoded one by up to about 2 of 16 grey levels on some pixels. Stock itself shows both variants,
depending on its cache state (see [RESULTS](docs/RESULTS.md#parity)).

## How it works

![Architecture](docs/img/architecture.svg)

- **LD_PRELOAD.** One line is added to the `kppmainapp` upstart job: unless `/mnt/us/DISABLE_KPPFAST` exists it runs
  `launch.sh`, which verifies every library against `SHA256SUMS` and `exec`s `/app/bin/KPPMainApp` with `LD_PRELOAD`.
  Amazon's files on disk, `/opt/amazon` and `cc.db` are not modified.
- **Interposed functions.** `libkppfs` memoises `stat` results and lookups of known-missing files, `libkppx11` moves
  image transfers to X11 shared memory, `libkppmalloc` gives glibc 2.20 per-thread free lists, `libkppimg` replaces a
  slow gray→RGB loop with NEON, `libkppdb` reuses prepared SQLite statements, `libkppside` caches one device-capability
  query and (optional switch) skips experiment-exposure records, `libkpplog` silences log spam with one switch.
- **Byte-verified in-memory patches.** `libkpphot` and `libkppcc` change a few instructions (a jump, an immediate) in
  the loaded code, in memory only, after the file size and the exact bytes of every site match. The expected bytes are
  **not shipped**: `libkpphot` needs them generated from your own device (`tools/genbytes.py`), `libkppcc` checks a
  hash. Any mismatch skips the patch and the original runs.
- **Amazon's own caches un-starved.** The biggest gain came from stock caches, not new code: on this device the
  cover-tile cache is capped at 2 MB (≈6 pages) and the whole-item cache at 1 MB (2–3 pages). `libkppcc` raises both
  to 16 MB and mirrors finished cache files to flash so they survive reboots. (The firmware itself picks larger limits
  on devices it considers bigger: 16 MB for the cover cache, 35 MB for the item cache.)
- **Text selection.** The touch driver's grip suppression swallows a finger lift near the right and bottom edges
  (≈40 px), so selecting across a page hung. `libkppgrip` switches suppression off for the duration of the gesture
  only; `KPPHOT_RSEL_EDGE=700` shortens the edge hold from 1000 to 700 ms. See the
  [diagram](docs/img/cross-page-selection.svg). The results were measured with `src/kppgrip/kppgrip.c` (v1); the hardened
  rewrite `kppgrip-v2-experimental.c` did **not** pass its first device test (see
  [RESULTS](docs/RESULTS.md#cross-page-text-selection-reader)).

![Where the time goes](docs/img/where-time-goes.svg)

## Safety

- Every patch has its own `KPP*=1` switch, off unless set. The installer's `install/env.default` turns on only the
  `libkppfs` and `libkppx11` switches.
- `launch.sh` starts stock if: `/mnt/us/DISABLE_KPPFAST` exists, 2 failed starts, 4 starts within 30 min, a library
  does not match `SHA256SUMS`. A hang (no LIPC answer in 180 s) counts as a failure.
- Without a computer: KUAL → "Safe mode" (reboot, accelerator off/on, everything off).
- Full rollback: `sh /var/local/kppfast/rollback.sh --restart` restores the stock upstart job and removes
  `/var/local/kppfast`.

## Parity

A speed-up counts only if screens and behaviour match stock: screenshots on the same pages (Library, Home, a book
page, quick settings; status bar masked), cache files compared, a behaviour pass (sort, list view, filter). The one
accepted difference is the cached-tile variant described above. See [METHODOLOGY](docs/METHODOLOGY.md).

## Amazon services and telemetry

Nothing here touches DRM, licensing, registration, purchases, sync or the network. Two switches change what the app
reports about itself, both off unless set: `KPPSIDE_WEBLAB=1` answers the app's local request to record A/B-experiment
("weblab") exposure triggers with an empty success instead of passing it on (experiment lookups are unchanged), and
`KPPLOG=1` drops the app's own WARNING-and-below log lines. Both were part of the measured configuration; the
`libkppside` gain was measured only together with its other switch. If you do not want to change what the device
reports, leave `KPPSIDE_WEBLAB` off. (`libkppio`, a diagnostic that is not in the measured configuration, times file
reads, including reads of book files, and can enlarge their read buffer; it never looks at or stores their content.)

## What did not work (honest list)

- `KPPLOG=2` (also dropping ~60 freeze-detector lines per turn): no gain.
- `KPPCC_LZ4=1` (own fast LZ4 decoder for cache tiles): no gain; the LZ4 time in the profile was compression on first visits.
- Faster quick settings (`libkppqs`, shorter window timers): caused a second screen update, rejected.
- Patching the Java reader: on the device books are rendered by KUX inside KPPMainApp, the Java reader is not used.
- Rebuilding cairo, swapping pixman/libjpeg, a thread pool instead of `std::thread`: more risk than gain, not done.
- `libkppio` (buffered reads) is built and unit-tested but was not measured on the device; not in the baseline.
- The first ever visit of a page (~0.42 s) still decodes, scales and draws every cover and reads it back from X to
  fill the caches; pre-filling the caches was not attempted.
- Book opening: not sped up. A warm open of an already-opened book takes ~1.24 s (tap to page fully shown);
  ~0.5 s of it is the Java reader (creating the book object, then notifying ~10 listeners) before KPPMainApp may
  show the page, ~0.34 s is the e-ink refresh. Sending KPPMainApp's open request earlier saved only ~45 ms (−4 %),
  because the page is still held back by the listeners; loading navigation data lazily gained nothing. Running the
  non-display listeners after the page is shown (expected −10–12 %) was built but never validated. None of this is
  shipped; it would require patching Amazon's Java code.
- Reader page turns: ~0.115 s of software, then ~0.37 s of e-ink refresh; nothing left to gain without changing
  the waveform (not done).
- Wake from sleep (power button): page clean after ~1.0 s, settled at ~1.3 s; almost all of it is two full e-ink
  refreshes (the second one removes ghosting of the screensaver cover). Dropping it would save ~0.45 s at the cost of
  possible ghosting; not done. The kppfast libraries and the other additions add no measurable delay on wake.

## Install

Kindle Oasis 3 on firmware 5.18.2 only, entirely at your own risk. Root access ("jailbreaking") is not part of this
project, is not explained and is not linked. How the author set it up: [docs/REPRODUCE.md](docs/REPRODUCE.md).

## Layout

`src/` library sources · `tools/` expected-bytes generator, Hermes bytecode tool · `install/` launcher, installer,
rollback · `bench/` benchmark harness · `safemode/` KUAL extension · `docs/` methodology, results, images ·
`build.sh` Docker build.

## License

[PolyForm Noncommercial 1.0.0](LICENSE.md), see also [NOTICE](NOTICE.md). You may use, modify and share the code for
noncommercial purposes under its terms; whoever passes on a copy (changed or not) must include the license terms (or
their URL) and the `Required Notice:` line below. Commercial use is not licensed. A link back to this repository is
appreciated. The license covers only the author's original code and documentation and grants no rights to Amazon
firmware, binaries, books or trademarks.

Required Notice: Copyright 2026 Artyom Lemelson (https://github.com/Lemelson)
