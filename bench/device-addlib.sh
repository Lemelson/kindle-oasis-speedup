#!/bin/sh
# Mac side: copy a built .so into /var/local/kppfast/lib on the Kindle and register its sha256 (launch.sh refuses
# unregistered or mismatching libraries). Does not change preload.list/env.  usage: device-addlib.sh path/libkppXYZ.so
H=${KINDLE:-kindle}; D=/var/local/kppfast; f=$1; b=$(basename "$f")
case "$b" in libkpp*.so) ;; *) echo "name must be libkpp*.so"; exit 2 ;; esac
ssh $H 'mkdir -p /mnt/us/kpp2' && scp -q "$f" $H:/mnt/us/kpp2/$b.new || exit 1
ssh $H "cp /mnt/us/kpp2/$b.new $D/lib/$b.tmp && chmod 644 $D/lib/$b.tmp && mv -f $D/lib/$b.tmp $D/lib/$b && grep -v ' $b\$' $D/lib/SHA256SUMS > $D/lib/S.tmp && (cd $D/lib && sha256sum $b) >> $D/lib/S.tmp && mv -f $D/lib/S.tmp $D/lib/SHA256SUMS && grep ' $b\$' $D/lib/SHA256SUMS"
