# Reproducing on your own Kindle / Воспроизведение на своём Kindle

[English](#english) · [Русский](#русский)

---

## English

> **Disclaimer.** Educational and research documentation of the author's personal experiments on his own device.
> This is not an encouragement or an instruction to modify devices. Modifying a Kindle may violate Amazon's terms of
> use, voids the warranty and can brick the device. No support is provided. You act entirely at your own risk. The
> project does not circumvent DRM or access protected content and must not be used to do so. Respect Amazon's
> copyrights and trademarks. Not affiliated with or endorsed by Amazon.

### Responsible use

- Only on a device you own. Only Kindle Oasis 3 on firmware 5.18.2: everything else is refused by the checks or untested.
- Never redistribute Amazon files: not the device libraries you link against, not the generated `hot_bytes.h`,
  not symbol dumps, not screenshots of your books. `.gitignore` excludes them; keep it that way in forks.
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

This is the hash-only variant: it writes no firmware bytes. If anything differs, stop: the offsets in this project
do not apply to your binaries, and the runtime checks would refuse every patch anyway.

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

    ssh kindle 'sh /var/local/kppfast/kppfast-ctl.sh set KPPFS_STAT=1'      # then: kppfast-ctl.sh restart

Order that worked here: fs + x11 → malloc + img → hot (each `KPPHOT_*` separately) → side → db → log → cc
(`KPPCC=1`, then `KPPCC_CAP_MB=16`, then `KPPCC_BOOK_MB=16 KPPCC_BOOK=1`) → grip. Add a library to the device with
`bench/device-addlib.sh out/libX.so`. `bench/device-baseline.sh` shows the full final configuration.

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

- Instantly, without a computer: KUAL → "Безопасный режим" → accelerator off (install `safemode/extension` as a KUAL
  extension first).
- Over USB: create an empty file `DISABLE_KPPFAST` in the root of the Kindle drive and restart.
- Completely: `ssh kindle 'sh /var/local/kppfast/rollback.sh --restart'` (stock upstart job back, directory removed).

### Java / Hermes

The shipped speed-up does not patch Java or Hermes bytecode. `tools/hbc.py` is a research tool for Hermes bytecode
v84 that edits a copy you extract from your own device; never publish original or patched bundles.

---

## Русский

> **Отказ от ответственности.** Образовательная и исследовательская документация личных экспериментов автора на
> его собственном устройстве. Это не призыв и не инструкция изменять устройства. Изменение Kindle может нарушать
> условия использования Amazon, лишает гарантии и может превратить устройство в «кирпич». Поддержки нет. Вы
> действуете целиком на свой риск. Проект не обходит DRM, не даёт доступа к защищённому контенту и не должен для
> этого использоваться. Уважайте авторские права и товарные знаки Amazon. Проект не связан с Amazon и не одобрен ею.

### Ответственное использование

- Только на своём устройстве. Только Kindle Oasis 3 на прошивке 5.18.2: на другом проверки откажут или ничего не проверено.
- Не распространяйте файлы Amazon: ни библиотеки с устройства для линковки, ни сгенерированный `hot_bytes.h`, ни
  дампы символов, ни снимки экрана со своими книгами. `.gitignore` их исключает, сохраните это в форках.
- Не используйте ничего отсюда, чтобы добраться до содержимого книг или снять защиту. Здесь этого нет.
- До начала подготовьте путь назад (kill switch, безопасный режим KUAL, `rollback.sh`) и включайте по одному переключателю.

### Порядок

0. **Условия.** Kindle Oasis 3, прошивка 5.18.2, **уже взломанный вами самостоятельно**, с root-доступом (SSH или
   USB-сеть) и KUAL. Инструменты джейлбрейка проект не даёт и на них не ссылается. Нужны Docker и Python 3.
1. **Свои файлы с устройства** (приватно, не коммитить): `libc.so.6`, `libpthread.so.0`, `libdl.so.2` из `/lib` в
   `~/kindle-private/klibs`; для `libkpphot` ещё патчимые бинарники (список в `tools/genbytes.py`) в `~/kindle-private/syms`.
2. **Проверка прошивки только по хэшам:** `python3 -I tools/genbytes.py --hash ~/kindle-private/syms` и сравнение с
   `docs/EXPECTED-HASHES.txt`. Любое отличие означает стоп.
3. **Сборка:** `python3 -I tools/genbytes.py ~/kindle-private/syms ~/kindle-private/hot_bytes.h`, затем
   `KLIBS=~/kindle-private/klibs HOTBYTES=~/kindle-private/hot_bytes.h ./build.sh test`. Без `HOTBYTES` собирается
   всё, кроме `libkpphot.so`. Библиотеки линкуются с glibc 2.20 вашего устройства (проверка: `readelf -V`).
4. **Установка лаунчера с первыми библиотеками:** `LIBS="libkppfs.so libkppx11.so" sh scripts/make-payload.sh`,
   скопировать `out/payload` на Kindle, `sh install.sh` от root, потом `sh /var/local/kppfast/check.sh`.
5. **Включать по одному:** `kppfast-ctl.sh set KEY=1` и `kppfast-ctl.sh restart`. Порядок: fs + x11 → malloc + img →
   hot (каждый `KPPHOT_*` отдельно) → side → db → log → cc → grip. Полная итоговая конфигурация в `bench/device-baseline.sh`.
6. **Замер A/B:** `bench/ab-run.sh` с чередованием B N B N, `bench/compare.py`. Выигрыш засчитывается, только если
   повторился в обоих кругах.
7. **Проверка экранов:** снимки одних и тех же страниц с кандидатом и без, строка состояния замаскирована,
   сравнение попиксельно. Снимки не публиковать: на них ваша библиотека.
8. **Откат:** KUAL → «Безопасный режим» → выключить ускоритель; или пустой файл `DISABLE_KPPFAST` в корне диска
   Kindle по USB; или полностью `sh /var/local/kppfast/rollback.sh --restart`.

Java и Hermes: итоговое ускорение байт-код не патчит. `tools/hbc.py` это исследовательский инструмент для Hermes
bytecode v84, работающий с копией, извлечённой с вашего собственного устройства; оригинальные или изменённые бандлы
не публиковать.
