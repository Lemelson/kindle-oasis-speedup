# Libraries

Each library is preloaded into Amazon's `KPPMainApp` by `install/launch.sh` and does nothing unless its switches are
set in `/var/local/kppfast/env`. Inside any other process the interposers stay inert. Build: `../build.sh`.
"Baseline" = the switch values of the final measured configuration (`../bench/device-baseline.sh`).

| library | what it does | baseline switches | other switches |
|---|---|---|---|
| `kppfs` | memoises `stat` under /mnt/us (TTL) and ENOENT for fixed missing config paths | `KPPFS_STAT=1 KPPFS_TTL_MS=1000 KPPFS_NEG=1 KPPFS_CLOCK=0` | `_NEG_TTL_MS _PREFIX _CLOCK_RT _CLOCK_WIN_MS _LOG _STATS` |
| `kppx11` | `XPutImage`/`XGetImage` through MIT-SHM; cover read-back from the GdkWindow | `KPPX11_SHMPUT=1 KPPX11_SHMGET=1 KPPX11_PIXBUFWIN=1` | `_DRAWPB _PIXBUF _VERIFY _MIN _SHMMODE _LOG _STATS` |
| `kppmalloc` | per-thread LIFO free lists for 16–1024 byte chunks on top of glibc 2.20 (no tcache there) | `KPPMALLOC=1` | `_LIMITS _ANYEXE _STATS` |
| `kppimg` | NEON `grayscale2RGB` replacement (exported function, PLT-interposed) | `KPPIMG=1` | |
| `kpphot` | in-memory patches after exact size + byte check: NEON gray8 conversion in GDK, cairo XImage read fast path, libgcc division in pixman, memoised `sin` in the Lanczos kernel, KUX selection edge hold | `KPPHOT_GDKGRAY=1 KPPHOT_CAIROGET=1 KPPHOT_PIXDIV=1 KPPHOT_PIXLDIV=1 KPPHOT_SINMEMO=1 KPPHOT_RSEL_EDGE=700` | `_RSEL_THROTTLE _RSEL_REPEAT _RSEL_PANFIX _LZ4ACCEL _LOG` |
| `kppside` | A/B-experiment ("weblab") exposure records answered locally with an empty success instead of being passed on (lookups untouched); `isColorEnabled` asked once | `KPPSIDE_WEBLAB=1 KPPSIDE_DEVCAP=1` | `_LOGDROP _TRACE _STATS` |
| `kppdb` | reuse of prepared SQLite statements of the Library queries | `KPPDB_STMTCACHE=1` | `_STMTCACHE_MAX _WINDOW _QUIETLOG _STATS` |
| `kpplog` | one switch for KPPMainApp log spam; `touch /var/local/kppfast/verbose-logs` restores every line without restart | `KPPLOG=1` | `KPPLOG=2` (no gain), `_STATS` |
| `kppcc` | Amazon's cover cache 2 → 16 MB and whole-item cache 1 → 16 MB (hash-checked immediate rewrite), finished cache files mirrored to flash and restored at start | `KPPCC=1 KPPCC_CAP_MB=16 KPPCC_BOOK_MB=16 KPPCC_BOOK=1` | `_LZ4 (no gain) _MAX_MB _STORE _FSYNC _LOG` |
| `kppgrip` | turns the touch driver's grip suppression off between a long-press and the release, so a lift at the edge is delivered. **This source (v2) failed its first device test; the baseline was measured with v1, which is not in this repository** | `KPPGRIP=1` (v1) | `_MAX_MS _FOCUS_MS _GATE _WIN _XEV _STATE _LOG` |
| `kppio` | times blocking file I/O per burst (diagnostic) and optional bigger stdio buffer / read-ahead for book files (built and unit-tested, not measured on the device) | (off) | `KPPIO _STAT _TRACE _GAP_MS _BUF_KB _RA_KB _RA_TAIL_KB _LOG _LOG_MAX_KB` |
| `kppprof` | SIGPROF sampling profiler for measurements only | (off) | `KPPPROF _DIR _US` |

`kpphot` needs `hot_bytes.h`, generated from your own device with `../tools/genbytes.py`; the repository ships no
firmware bytes. `kppcc` checks its three patch sites by FNV-1a-64 hash plus file size.
Unit tests that need no device: `kppgrip/griptest.sh`, `kppcc/cctest6.sh`, `kppio/iotest.sh` (`../build.sh test`).
`kppfs/selftest.c`, `kppmalloc/malloctest.c`, `kppimg/imgtest.c`, `kppdb/*test.c`, `kpplog/logtest.*`, `kpphot/hottest.c`,
`kppcc/lz4test.c` need the firmware's own libraries (a rootfs image of your device) and are provided as reference;
`build.sh` does not build or run them.
