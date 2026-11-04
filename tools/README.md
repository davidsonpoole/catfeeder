# tools

Host-side tools for the cat feeder, and how to get firmware onto it. The feeder
itself is an ESP32 running `src/main.cpp`, which serves a JSON API over HTTP on
port 80.

## catfeeder

`catfeeder` is the client. Python 3, standard library only, no install step:

    tools/catfeeder status

Every command takes the feeder's address from `--host`, else `$CATFEEDER_HOST`,
else `192.168.1.244`. Add it to your `PATH` or alias it if you use it often.

    catfeeder status                    the clock and the meal schedule
    catfeeder feed [N]                  dispense N portions now (default 1)
    catfeeder meals                     list the schedule
    catfeeder meals add 07:30 2         two portions at 07:30 every day
    catfeeder meals modify 07:30 3      make the 07:30 meal three portions
    catfeeder meals rm 07:30            delete the 07:30 meal
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
is and **holds every meal until it has one**:

    $ catfeeder status
    clock:  NOT SYNCED - no meals until NTP answers, or for 48m more before the feeder guesses (NTP needs a route to the internet)

It fixes that itself. Once WiFi is up it asks `pool.ntp.org` for the time over
NTP. Until the first answer it retries every 15 seconds, backing off to every
150, and never gives up. Once it has an answer it re-polls hourly for the rest of
its life, so the clock is also trued up against the crystal's drift without
anyone doing anything. A clock last synced over 14 days ago shows `[STALE]`.

**If NTP has not answered 50 minutes after boot,** the feeder stops waiting and
feeds by a guessed clock. Every 10 minutes it writes the time to flash, and
after a power cut the guess picks up from the last time it saved. The guess
runs late by however long the power was off, but meals still come at the right
intervals:

    $ catfeeder status
    clock:  GUESSED local 06:12, UTC-05:00 - NTP has not answered, so meals follow the feeder's own clock (late by however long the power was off)

It keeps asking NTP the whole time. When the real time arrives, each meal served
on the guessed clock counts for whichever real mealtime it was closest to, so
nothing is served twice. With a 07:30 breakfast, if the guess served it at 01:30
real time, that was today's breakfast and 07:30 is skipped. If it served it at
21:30, that was tomorrow's breakfast.

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

### The schedule

Meals are a time of day plus a portion count, up to 8 of them, kept in flash.
The schedule is always in time order, so a new meal drops into place rather than
onto the end.

There can be only one meal at a given time, so meals are addressed by their
time: adding or removing one never changes how you name the others.

    $ catfeeder meals rm 12:00
    removed 12:00  1 portion

    $ catfeeder meals modify 07:30 3
    modified 07:30  3 portions (was 2)

Only the portions change, so the meal keeps its place and its record of having
fed today — raising the portions after breakfast has gone out changes tomorrow's
breakfast, it does not serve a second one. A time with no meal on it is refused
rather than quietly turned into a new meal, so a typo cannot add one:

    $ catfeeder meals modify 08:15 3
    catfeeder: no meal scheduled at 08:15 - `catfeeder meals` lists the times

`rm` refuses an empty time the same way. To change a meal's *time*, there is
still nothing for it but `rm` and `add`. Adding a second meal at a time that
already has one is refused:

    $ catfeeder meals add 07:30 2
    catfeeder: there is already a meal at 07:30 - `catfeeder meals modify` changes its portions

If a write reaches the schedule but not flash, the output says
`[NOT SAVED to flash]`: the change is live but will not survive a reboot.

### Catching up after an outage

Flash records the day each meal last fed, written the moment it is served. A
feeder that comes back mid-day therefore knows exactly what the cat has already
had, and **serves whatever it missed, straight away**:

    14:01:58  Clock synced from NTP: local time 14:01 (UTC-4:00)
    14:01:59  MealEvent: serving 1 portion(s) scheduled for 12:00, 2h01m late (its time went by while the feeder was down)
    14:02:04  MealEvent: served 1 of 1 portion(s)

A meal that already fed before the restart is not served again — flash says it
fed today, and that is the end of it. `catfeeder status` shows which are done:

    $ catfeeder status
    clock:  local 14:02, UTC-04:00, synced 1m ago
    meals:  2 scheduled
      07:30  2 portions  fed today
      12:00  1 portion   fed today

Catch-up is oldest first, since the schedule is in time order. Note that it has
no window: a feeder that was off from breakfast until the evening serves every
meal it missed, back to back, as soon as it has the time. The hopper empties into
the bowl rather than the cat going without.

A meal **added** after its own time today is a different case and is not caught
up: it did not exist when its slot went by, so it is stamped as settled for today
and starts tomorrow.

The one hole is a flash write that fails at the moment of feeding, which logs
`fed, but could not record it to flash`. A restart the same day would then feed
that meal a second time.

## Live logs

Everything the feeder logs to its serial console is also broadcast on UDP port
3957, so the log can be followed without a cable:

    $ catfeeder logs
    09:14:01  following the log on UDP port 3957
    09:14:07  Clock synced from NTP: local time 09:14 (UTC-4:00)
    09:29:58  192.168.1.9 GET /api/meals -> 200
    09:30:00  MealEvent: serving 2 portion(s) (scheduled for 07:30)
    09:30:04  Dispense: portion 1 of 2
    09:31:12  192.168.1.9 POST /api/meals -> 201
    09:31:40  192.168.1.9 DELETE /api/meals/09:15 -> 404

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

## Flashing

Over USB:

    pio run -t upload

Add `--upload-port /dev/cu.usbserial-0001` when more than one serial device is
attached, and `pio device monitor` to follow the serial console afterwards.

**Do not leave USB connected while the board runs from an external supply**,
unless its schematic shows proper supply ORing. On many dev boards USB 5V and the
`5V` pin are tied together and the two sources fight, backfeeding the laptop's
port; driving the `3V3` pin while USB is connected pushes current backwards
through the onboard regulator's output, which most regulators do not survive.
Power from one source at a time: USB while developing, external once deployed.

### Over WiFi

    export CATFEEDER_OTA_PASSWORD='...'        # the value is in src/wifi_secrets.h
    pio run -e esp32dev_ota -t upload

The feeder answers to `catfeeder.local`, set by `OTA_HOSTNAME` in `src/main.cpp`.
Where mDNS does not work, give the address instead:

    pio run -e esp32dev_ota -t upload --upload-port 192.168.1.244

The password lives in `src/wifi_secrets.h`, which is gitignored, and
`platformio.ini` reads it from the environment so it never lands in the repo.
Without one, anyone on the network could install their own firmware on the
feeder, which is a good deal worse than being able to dispense a portion.

An update arriving mid-meal waits for the meal: `ArduinoOTA.handle()` is only
reached from `loop()`, and a dispense does not return there until it has counted
its portions. The motor is stopped before the write regardless, so an update
landing between portions cannot leave the auger turning through the reboot.

A failed upload costs nothing. `otadata` still points at the slot the feeder is
running from, so it keeps running the firmware it has and you can retry.

### What still needs a cable

    nvs       0x009000 .. 0x00E000   the saved schedule, and what has been fed today
    otadata   0x00E000 .. 0x010000   which app slot to boot
    app0      0x010000 .. 0x200000   1.94 MiB
    app1      0x200000 .. 0x3F0000   1.94 MiB
    coredump  0x3F0000 .. 0x400000

An OTA writes only the app slot the feeder is *not* running from. The bootloader
at `0x1000` and the partition table at `0x8000` are never touched, so editing
`partitions.csv` means a USB flash -- as does recovering firmware that does not
boot. A flash leaves `nvs` alone, so the schedule and today's feeding record
survive both kinds of update.

**Firmware with the wrong WiFi credentials cannot be fixed over the air.** That
is the one way to lose a feeder you have put somewhere awkward. Commit before you
push, so whatever is running is something you can rebuild.
