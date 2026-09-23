#!/bin/bash
# Pushes this laptop's clock to the cat feeder.
#
# The feeder has no RTC battery and no route off the LAN, so this is how it
# learns what time it is. Run it daily (see tools/catfeeder-timesync.plist), and
# after any power cut -- until it is synced the feeder will not feed.
#
# Usage: tools/sync-time.sh [host]
set -euo pipefail

HOST="${1:-${CATFEEDER_HOST:-192.168.1.244}}"

epoch=$(date +%s)

# date +%z is like "-0700"; turn it into signed minutes east of UTC.
z=$(date +%z)
sign=${z:0:1}
offset=$(( 10#${z:1:2} * 60 + 10#${z:3:2} ))
[ "$sign" = "-" ] && offset=$(( -offset ))

curl -fsS --max-time 10 \
    -X POST "http://${HOST}/api/time" \
    -H 'Content-Type: application/json' \
    -d "{\"epoch\":${epoch},\"tzOffsetMinutes\":${offset}}"
echo
