#!/bin/bash
# Polls the portion sensor and prints only on a change, so a wire can be
# prodded with one hand and watched with the other.
#
# Usage: tools/watch-sensor.sh [host]

HOST="${1:-192.168.1.244}"
last=""

while true; do
    now=$(curl -s -m 2 "http://$HOST/api/sensor" |
          sed -n 's/.*"level":"\([A-Z]*\)".*/\1/p')
    [ -z "$now" ] && now="(no response)"

    if [ "$now" != "$last" ]; then
        printf '%s  %s\n' "$(date +%H:%M:%S)" "$now"
        last="$now"
    fi

    sleep 0.2
done
