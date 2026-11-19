#!/bin/sh
# Device side: make sure the screen is awake (not screensaver/suspend) before injecting input.
# powerd_test -p simulates a power-button press, which leaves the screensaver.
for i in 1 2 3 4; do
  st=$(lipc-get-prop com.lab126.powerd state 2>/dev/null)
  case "$st" in active) echo "wake: state=active"; exit 0 ;; esac
  echo "wake: state=$st, pressing power"
  powerd_test -p >/dev/null 2>&1
  sleep 4
done
st=$(lipc-get-prop com.lab126.powerd state 2>/dev/null)
echo "wake: final state=$st"
[ "$st" = active ]
