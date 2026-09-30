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

So sync after every power cut. `catfeeder-timesync.plist` does this daily; see
below. A clock synced more than 14 days ago is flagged `[STALE]`.

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

## catfeeder-timesync.plist

A launchd agent that runs `sync-time.sh` daily at 09:00, plus at load, so the
feeder's clock stays correct without anyone remembering to do it. Install:

    cp tools/catfeeder-timesync.plist ~/Library/LaunchAgents/local.catfeeder.timesync.plist
    launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.catfeeder.timesync.plist

The plist has the path to this checkout baked in; edit it if the repo moves.
Output goes to `/tmp/catfeeder-timesync.log`.

## sync-time.sh

`catfeeder sync-time` does the same job. This one stays because the launchd
agent runs it, and because curl is there on a machine with no Python.
