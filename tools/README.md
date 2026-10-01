# tools

Host-side tools for the cat feeder. The feeder itself is an ESP32 running
`src/main.cpp`, which serves a JSON API over HTTP on port 80.

## catfeeder

`catfeeder` is the client. Python 3, standard library only, no install step:

    tools/catfeeder status

Every command takes the feeder's address from `--host`, else `$CATFEEDER_HOST`,
else `192.168.1.244`. Add it to your `PATH` or alias it if you use it often.

    catfeeder status                    the clock and the meal schedule
    catfeeder feed [N]                  dispense N portions now (default 1)
    catfeeder time                      what the feeder thinks the time is
    catfeeder meals                     list the schedule
    catfeeder meals add 07:30 2         two portions at 07:30 every day
    catfeeder meals set 0 08:15 1       replace meal 0
    catfeeder meals rm 0                delete meal 0
    catfeeder logs                      follow the feeder's log stream
    catfeeder record                    write the log stream to a file per day

`--json` prints the raw response instead of a summary, for piping into `jq`.
`catfeeder <command> --help` covers the rest.

### Feeding

    $ catfeeder feed 2
    dispensed 2 of 2

The feeder drives the motor until the portion sensor counts each portion, so it
answers only once the food has actually moved — `feed` waits that out. If the
sensor comes up short the command says so and exits non-zero, which usually
means a jam or an empty hopper:

    $ catfeeder feed 2
    dispensed 1 of 2
    the sensor did not count every portion - check for a jam

`--timeout MS` changes how long the feeder waits for each portion to reach the
sensor (100-60000, default 10000).

### The clock

The feeder has no RTC battery, so after a power cut it has no idea what time it
is and **will not feed until it has one**:

    $ catfeeder status
    clock:  NOT SYNCED - the feeder will not feed until NTP answers (needs a route to the internet)

It fixes that itself. Once WiFi is up it asks `pool.ntp.org` for the time over
NTP, and keeps asking in the background for the rest of its life, so the clock
is also trued up against the crystal's drift without anyone doing anything. A
clock last synced over 14 days ago shows `[STALE]`.

NTP answers in UTC only. The feeder turns that into local time using a zone
rule compiled into the firmware, `POSIX_TZ` in `src/main.cpp`:

    EST5EDT,M3.2.0,M11.1.0

That is America/New_York: five hours west of UTC (POSIX inverts the sign),
daylight time from the second Sunday in March to the first in November. The
switchovers are rules rather than dates, so they are worked out arithmetically
for any year — no calendar data to ship and nothing to keep up to date. The
feeder re-checks the rule every second, so it follows a switchover within a
second of it happening even if the network is down. **If the law ever moves
those dates, that line is what has to be reflashed.** Moving the feeder to
another zone is the same one-line change.

`catfeeder time` shows what it has settled on, including the rule in `--json`:

    $ catfeeder time
    clock:  local 09:14, UTC-04:00, synced 2m ago

**If there is no route to the internet,** NTP cannot answer and the feeder stays
parked. `POST /api/time` is the manual way out:

    curl -X POST http://192.168.1.244/api/time \
        -H 'Content-Type: application/json' \
        -d "{\"epoch\":$(date +%s)}"

It takes a UTC epoch and nothing else — the zone is the feeder's own business,
so there is no offset to get wrong. NTP will correct whatever you push the next
time it is reachable.

### The schedule

Meals are a time of day plus a portion count, up to 8 of them, kept in flash.
The schedule is always in time order, so adding a meal, or editing one's time,
drops it into place rather than onto the end.

Meals are addressed by their position in that order, which means **any edit can
renumber the others** — run `catfeeder meals` between edits rather than working
from a stale listing.

A meal already served today stays served, so moving its time later in the day
does not feed the cat twice. If a write reaches the schedule but not flash, the
output says `[NOT SAVED to flash]`: the change is live but will not survive a
reboot.

## Live logs

Everything the feeder logs to its serial console is also broadcast on UDP port
3957, so the log can be followed without a cable:

    $ catfeeder logs
    09:14:01  following the log on UDP port 3957
    09:14:07  Clock synced from NTP: local time 09:14 (UTC-4:00)
    09:29:58  192.168.1.9 GET /api/settings -> 200
    09:30:00  MealEvent: serving 2 portion(s) (scheduled for 07:30)
    09:30:04  Dispense: portion 1 of 2
    09:31:12  192.168.1.9 POST /api/meals -> 201
    09:31:40  192.168.1.9 PUT /api/meals/9 -> 404

Every API call is logged that way — who asked, what for, and what they got —
including the ones refused before a handler ran, so a client getting a 400 shows
up here with the reason it was given. Note that this makes the log as busy as the
API is: anything polling the feeder in a loop will say so, once per request.

Broadcast, so viewers come and go without the feeder knowing, and a listener
that is not there can never block the scheduler waiting to be read. The cost is
that UDP can drop a line, so the feeder numbers them and the viewer says when
the count jumps:

    09:31:12  -- 2 log lines lost --
    09:35:40  -- the feeder restarted --

Several viewers can run at once, each getting a copy, because the lines are
broadcast rather than handed to one reader. Lines logged before WiFi is up —
restoring the schedule from flash, the connect dots — only reach the serial
console, which is still there and unchanged.

### Keeping the log

`catfeeder logs` shows the stream but stores nothing; close it and the log is
gone. `catfeeder record` writes it to a file per day, in `logs/` beside this
checkout:

    $ catfeeder record
    09:14:01  recording the log to /Users/davidson/git/self/catfeeder/logs
    09:14:01  writing to /Users/davidson/git/self/catfeeder/logs/09-30-26.log

    $ tail -2 logs/09-30-26.log
    09:30:00  MealEvent: serving 2 portion(s) (scheduled for 07:30)
    09:30:04  Dispense: portion 1 of 2

The date is in the filename, so each line carries only the time. It rotates at
midnight, appends rather than truncating, and writes a line at a time so
`tail -f` keeps up. `--dir` puts the files somewhere else; `logs/` is
gitignored. Run it resident with `catfeeder-logger.plist`, below, and it shares
the port, so `catfeeder logs` still works alongside it.

## catfeeder-logger.plist

A launchd agent that keeps `catfeeder record` running, so the log is on disk
without anyone remembering to start it. Install:

    cp tools/catfeeder-logger.plist ~/Library/LaunchAgents/local.catfeeder.logger.plist
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.catfeeder.logger.plist

Its own output — which file it is writing to — goes to
`/tmp/catfeeder-logger.log`. Nothing prunes `logs/`, so it grows for as long as
the feeder keeps talking; a few hundred kilobytes a year at the current rate.
