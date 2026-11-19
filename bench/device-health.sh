#!/bin/sh
# Mac side: is the Kindle healthy enough to experiment on / was it left healthy?  exit 0 = OK, 1 = problem, 2 = unreachable
#   device-health.sh            check and print
#   device-health.sh --heal     if the GUI stack is broken (a job down, e.g. after an Xorg crash), reboot and re-check
# Checks: all GUI upstart jobs running, KPPMainApp up, free space on /var /tmp /var/local /mnt/us, battery,
# Amazon crash counter, kppfast fail-safe flag, stray crash reports in the library.
H=${KINDLE:-kindle}
out=$(ssh -o ConnectTimeout=8 $H 'bad=0
for j in lab126_gui x framework pillow statusbar kppmainapp; do s=$(status $j 2>&1 | head -1); case "$s" in *start/running*) ;; *) echo "JOB $j: $s"; bad=1 ;; esac; done
pidof KPPMainApp >/dev/null || { echo "KPPMainApp not running"; bad=1; }
for m in /var:8000 /tmp:8000 /var/local:20000 /mnt/us:300000; do d=${m%%:*}; need=${m##*:}; free=$(df -k $d 2>/dev/null | awk "NR==2{print \$4}"); [ "${free:-0}" -ge $need ] || { echo "LOW SPACE $d: ${free}k free (< ${need}k)"; bad=1; }; done
b=$(lipc-get-prop com.lab126.powerd battLevel 2>/dev/null); c=$(lipc-get-prop com.lab126.powerd isCharging 2>/dev/null)
[ "${b:-0}" -ge 15 ] || [ "$c" = 1 ] || { echo "BATTERY $b% not charging"; bad=1; }
cc=$(cat /tmp/kppmainapp_crash_counter.txt 2>/dev/null); [ "${cc:-0}" -ge 2 ] && { echo "Amazon crash counter $cc"; bad=1; }
[ -e /var/local/kppfast/disabled ] && echo "note: kppfast fail-safe active: $(cat /var/local/kppfast/disabled)"
n=$(ls /mnt/us/documents 2>/dev/null | grep -c "^KPPMainAppV2_"); [ "$n" -gt 0 ] && echo "note: $n crash report(s) in documents"
echo "battery=$b% charging=$c uptime=$(cut -d. -f1 /proc/uptime)s"
exit $bad' 2>&1); rc=$?
echo "$out"
[ $rc = 255 ] && { echo "UNREACHABLE"; exit 2; }
# transient: pillow restarts itself once after every KPPMainApp restart ("start/post-start" for a few seconds)
if [ $rc = 1 ] && [ -z "$HEALTH_RETRY" ] && ! echo "$out" | grep -v "JOB pillow: pillow start/" | grep -q "^JOB \|KPPMainApp not running\|LOW SPACE\|BATTERY\|crash counter"; then
  sleep 8; HEALTH_RETRY=1 exec sh "$0" "$@"
fi
if [ $rc != 0 ] && [ "$1" = --heal ] && echo "$out" | grep -q "^JOB \|KPPMainApp not running"; then
  echo "GUI stack broken -> rebooting the Kindle"; ssh $H 'sync; (sleep 2; reboot) >/dev/null 2>&1 &'
  sleep 30; until ssh -o ConnectTimeout=5 $H 'test $(cut -d. -f1 /proc/uptime) -gt 120 && pidof KPPMainApp' >/dev/null 2>&1; do sleep 10; done
  exec sh "$0"
fi
[ $rc = 0 ] && echo HEALTHY || echo UNHEALTHY; exit $rc
