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
    catfeeder sync-time                 push this computer's clock to the feeder
    catfeeder meals                     list the schedule
    catfeeder meals add 07:30 2         two portions at 07:30 every day
    catfeeder meals set 0 08:15 1       replace meal 0
    catfeeder meals rm 0                delete meal 0
    catfeeder serve-time                answer the feeder's requests for the time
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

The feeder has no RTC battery and no route off the LAN, so it learns the time
from this computer and **will not feed until it has been synced**:

    $ catfeeder status
    clock:  NOT SYNCED - the feeder will not feed until it is (run: catfeeder sync-time)

It can be told the time two ways, and both are wanted.

**The feeder asks.** On startup, and every 30 seconds for as long as its clock
is unset, it broadcasts a request on UDP port 3956. `catfeeder serve-time`
answers with this computer's time:

    $ catfeeder serve-time
    09:14:02  answering sync requests on UDP port 3956
    09:14:07  192.168.1.244 asked for the time, sent 09:14:07 UTC-04:00

It broadcasts because this computer's address is not fixed and is the one that
moves; nothing has to be configured on the feeder. Run it resident with
`catfeeder-timeserver.plist` (below) and a feeder that reboots at 03:00 is
feeding again by 03:00, without anyone awake.

**Or you push.** `catfeeder sync-time` POSTs to `/api/time`, as before, and
`catfeeder-timesync.plist` does that daily. This is still the only way to
*correct* a clock that is already set: once synced, the feeder ignores sync
replies, so nothing on the network can quietly move a working clock. Drift is
what the daily push is for, and a clock synced over 14 days ago shows `[STALE]`.

Nothing answering is not an error. The feeder asks again in 30 seconds, for as
long as it takes.

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
    09:14:07  Clock synced from 192.168.1.9: local time 09:14 (UTC-4:00)
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

## catfeeder-timesync.plist

A launchd agent that runs `sync-time.sh` daily at 09:00, plus at load, so the
feeder's clock stays correct without anyone remembering to do it. Install:

    cp tools/catfeeder-timesync.plist ~/Library/LaunchAgents/local.catfeeder.timesync.plist
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.catfeeder.timesync.plist

The plist has the path to this checkout baked in; edit it if the repo moves.
Output goes to `/tmp/catfeeder-timesync.log`.

## catfeeder-timeserver.plist

A launchd agent that keeps `catfeeder serve-time` running, so there is always
something to answer the feeder when it comes back from a power cut. Install:

    cp tools/catfeeder-timeserver.plist ~/Library/LaunchAgents/local.catfeeder.timeserver.plist
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.catfeeder.timeserver.plist

macOS may ask once whether python3 may accept incoming connections. It needs
to, or the feeder's broadcast never arrives. Output goes to
`/tmp/catfeeder-timeserver.log`.

Being asleep is the one thing it cannot work around: a sleeping laptop answers
nothing, and the feeder waits, asking, until it wakes.

## catfeeder-logger.plist

A launchd agent that keeps `catfeeder record` running, so the log is on disk
without anyone remembering to start it. Install:

    cp tools/catfeeder-logger.plist ~/Library/LaunchAgents/local.catfeeder.logger.plist
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.catfeeder.logger.plist

Its own output — which file it is writing to — goes to
`/tmp/catfeeder-logger.log`. Nothing prunes `logs/`, so it grows for as long as
the feeder keeps talking; a few hundred kilobytes a year at the current rate.

## sync-time.sh

`catfeeder sync-time` does the same job. This one stays because the launchd
agent runs it, and because curl is there on a machine with no Python.
