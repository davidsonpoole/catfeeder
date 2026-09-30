#!/bin/bash
# Polls every candidate input and prints the set reading HIGH, on change only.
# Touch 3V3 to a wire and whichever pin it actually lands on shows up here --
# useful when the wire may not be in the header hole you think it is.
#
# Usage: tools/watch-pins.sh [host]

HOST="${1:-192.168.1.244}"
last=""

while true; do
    high=$(curl -s -m 2 "http://$HOST/api/pins" |
           tr ',' '\n' |
           sed -n 's/.*"\([0-9]\{1,2\}\)":"HIGH".*/\1/p' |
           tr '\n' ' ')
    [ -z "$high" ] && high="(none)"

    if [ "$high" != "$last" ]; then
        printf '%s  high: %s\n' "$(date +%H:%M:%S)" "$high"
        last="$high"
    fi

    sleep 0.2
done
