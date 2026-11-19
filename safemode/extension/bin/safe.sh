#!/bin/sh
# KUAL "Безопасный режим" (Safe mode): on-device recovery without a computer.
#   reboot       - reboot (fixes a broken GUI stack, e.g. after an Xorg crash)
#   kppfast-off  - /mnt/us/DISABLE_KPPFAST (Library starts exactly like Amazon's) + reboot
#   kppfast-on   - remove that file + reboot
#   all-off      - kppfast off and its fail-safe latched (/var/local/kppfast/disabled), then reboot.
#                  Add your own other on-device modifications here if you have any.
LOG=/mnt/us/safemode.log
say() { echo "$(date '+%F %T') $*" >> $LOG; eips 0 1 "$*                                  " >/dev/null 2>&1; }
case "$1" in
  reboot)      say "Безопасный режим: перезагрузка..." ;;
  kppfast-off) : > /mnt/us/DISABLE_KPPFAST; say "Ускоритель выключен, перезагрузка..." ;;
  kppfast-on)  rm -f /mnt/us/DISABLE_KPPFAST; say "Ускоритель включён, перезагрузка..." ;;
  all-off)
    : > /mnt/us/DISABLE_KPPFAST
    [ -d /var/local/kppfast ] && : > /var/local/kppfast/disabled
    say "Все доработки выключены, перезагрузка..." ;;
  *) say "safe.sh: неизвестная команда $1"; exit 2 ;;
esac
sync; sleep 2; reboot
