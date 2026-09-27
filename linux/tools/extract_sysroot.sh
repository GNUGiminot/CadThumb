#!/bin/bash
set -e
which dpkg-deb
rm -rf ~/sysroot && mkdir -p ~/sysroot
cd ~/aptroot/debs
ok=0
fail=0
for f in *.deb; do
  if dpkg-deb -x "$f" ~/sysroot 2>/tmp/dpkgerr; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    echo "FAIL $f: $(cat /tmp/dpkgerr)"
  fi
done
echo "extracted ok=$ok fail=$fail"
du -sh ~/sysroot
