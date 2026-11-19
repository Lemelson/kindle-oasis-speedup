# kppfast launcher

`install.sh` (on the Kindle, as root) adds ONE line to `/etc/upstart/kppmainapp.conf` before `exec /app/bin/KPPMainApp`:

    [ -e /mnt/us/DISABLE_KPPFAST ] || [ ! -f /var/local/kppfast/launch.sh ] || exec /bin/sh /var/local/kppfast/launch.sh # kppfast

It refuses unless the firmware is 5.18.2 and the job file is exactly the stock one (sha256). Everything else lives in
`/var/local/kppfast/`: `launch.sh`, `kppfast-lib.sh`, `kppfast-ctl.sh`, `check.sh`, `rollback.sh`, `lib/*.so` +
`lib/SHA256SUMS`, `preload.list`, `env`, fail-safe state, `launch.log`, a copy of the stock job.

## launch.sh (always ends in `exec /app/bin/KPPMainApp`, same pid for upstart)
- Stock start if: `/mnt/us/DISABLE_KPPFAST` | `/var/local/kppfast/disabled` | 2 failed starts | ≥ 4 preloaded starts
  within 30 min | a preload entry outside `lib/`, missing, or not matching `lib/SHA256SUMS` | empty list.
- `env` is parsed, never sourced: only `KPP*=[A-Za-z0-9_./:-]*` lines are exported.
- A start counts as good when the same pid lives ≥ 60 s and answers LIPC; no answer within 180 s → killed, counted
  as a failure. Two failures → the third start is stock and `disabled` is written.

## Commands (on the Kindle, root)

    sh install.sh [payload]                      # payload from scripts/make-payload.sh
    sh /var/local/kppfast/check.sh               # status, exit 0 = OK
    sh /var/local/kppfast/rollback.sh --restart  # stock job back, /var/local/kppfast removed
    touch /mnt/us/DISABLE_KPPFAST                # every following start is stock
    sh /var/local/kppfast/kppfast-ctl.sh stock|all|set KEY=VAL|unset KEY|enable|restart

Logs and job-file backups: `/mnt/us/kppfast-logs/`.
