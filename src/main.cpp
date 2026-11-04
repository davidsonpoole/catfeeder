#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <uri/UriBraces.h>

#include "wifi_secrets.h"

#define HTTP_PORT 80
#define MAX_MEALS 8
#define MAX_PORTIONS 10
#define MINUTES_PER_DAY 1440
#define SECONDS_PER_DAY 86400L

// The clock has no battery, so it means nothing until something has told us the
// time. Anything before 2020 is a device that has not been told yet.
#define MIN_VALID_EPOCH 1577836800LL  // 2020-01-01T00:00:00Z
#define CLOCK_STALE_SECONDS (14L * SECONDS_PER_DAY)

// WiFi comes and goes; the scheduler must not. Nothing on the reconnect path
// blocks, so a meal whose time arrives during an outage is still served on
// time: the clock is local and the motor needs no network. The ESP32
// reconnects on its own, but only to an AP that still looks the way it did, so
// we also re-issue begin() now and then for a router that came back different.
// The interval is generous because a begin() while an association is already in
// flight starts that association over.
#define WIFI_RETRY_INTERVAL_MS 30000

// What the feeder calls itself for an over-the-air update, so a flash is
// `--upload-port catfeeder.local` rather than an address that moves whenever the
// network does.
#define OTA_HOSTNAME "catfeeder"

// The feeder gets the time from the internet and works out local time itself,
// so nothing on the LAN has to be awake for the cat to be fed. NTP only ever
// answers in UTC; the zone rule below is what turns that into wall-clock time.
//
// EST5EDT,M3.2.0,M11.1.0 is America/New_York: 5 hours west of UTC (POSIX
// inverts the sign), daylight time from the 2nd Sunday in March to the 1st in
// November. The switchovers are rules rather than dates, so libc works them out
// arithmetically for any year and needs no calendar data and no network. If the
// law ever moves those dates, this line is what has to be reflashed.
#define POSIX_TZ "EST5EDT,M3.2.0,M11.1.0"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

// SNTP polls in the background on its own schedule; this is only how often
// loop() looks at what it has done.
#define NTP_CHECK_INTERVAL_MS 1000

// How often SNTP re-polls once it has a time. Until the first answer it retries
// on its own, every 15s backing off to 150s, and never gives up.
#define NTP_SYNC_INTERVAL_MS (60UL * 60 * 1000)

// How long to wait for NTP after boot before feeding by the feeder's own best
// guess at the time: 100 attempts at the WiFi retry interval, about 50 minutes.
// A meal at roughly the right time beats no meal; SNTP keeps trying regardless.
#define CLOCK_GUESS_AFTER_MS (100UL * WIFI_RETRY_INTERVAL_MS)

// How often the time is written to flash, so a feeder back from a power cut
// with no network can guess from where it left off rather than from nothing.
// NVS levels its wear, so this costs each page a few erases a week.
#define CLOCK_SAVE_INTERVAL_MS (10UL * 60 * 1000)

// Log lines are broadcast too, so a viewer can come and go without the feeder
// knowing or caring, and a missing listener can never block the scheduler.
#define LOG_PORT 3957
#define LOG_LINE_MAX 200

#define MEAL_NEVER_FIRED (-1L)

// A meal served this many minutes past its time is a catch-up rather than a
// meal on time, and is logged as one. The scheduler ticks every second, so an
// on-time meal is late by 0; the slack is for a meal that had to queue behind a
// long dispense.
#define MEAL_LATE_MINUTES 2

// DRV8833 channel A. The auger only ever turns one way, so IN2 stays low and
// IN1 does all the work; both low is the driver's coast mode.
#define MOTOR_IN1_PIN 22
#define MOTOR_IN2_PIN 23

// A button the dispenser closes once per portion, wired between this pin and
// ground: pressing it pulls the pin low, so the idle level has to come from a
// pull-up. This pin has one internally -- which is why it is not one of
// GPIO34-39, where there are no internal pulls at all and a pull mode is
// silently ignored, leaving the input to float.
#define PORTION_SENSOR_PIN 32
#define PORTION_SENSOR_ACTIVE_LEVEL LOW
#define PORTION_SENSOR_IDLE_LEVEL HIGH

// The sensor is a mechanical contact, so it rattles on both edges.
#define PORTION_DEBOUNCE_MS 50

// A portion that has not reached the sensor by now means a jam or an empty
// hopper. Give up rather than grind the motor against it for the rest of the
// day.
#define PORTION_TIMEOUT_MS 10000

// Ceiling on a per-portion timeout asked for by a /api/dispense request.
// Pressing the sensor by hand is much slower than an auger tripping it, so a
// bench test wants longer than a real meal should ever wait.
#define PORTION_TIMEOUT_MAX_MS 60000

// The schedule lives in NVS so it survives a power cut. Bump the version if the
// stored layout ever changes; a mismatch is treated as "no saved schedule".
#define NVS_NAMESPACE "catfeeder"
#define NVS_KEY_VERSION "ver"
#define NVS_KEY_MEALS "meals"
#define NVS_KEY_GUESSED "guessed"  // bitmask over the stored meals: stampGuessed
#define NVS_KEY_CLOCK "clock"      // last known UTC epoch, synced or guessed
#define SCHEDULE_FORMAT_VERSION 2

typedef struct {

    int portions;
    int timeOfDay;    // minutes since local midnight
    long lastFiredDay;

    // lastFiredDay was stamped while the clock was only a guess, so it counts
    // days on the guessed clock and has to be moved once the real time is known.
    bool stampGuessed;

} Meal;

// What goes to flash: the schedule, and what has actually been served off it.
typedef struct {

    int16_t portions;
    int16_t timeOfDay;
    int32_t lastFiredDay;

} StoredMeal;

WebServer server(HTTP_PORT);
Preferences prefs;

WiFiUDP logUdp;  // send only: nothing ever answers a log line

Meal meals[MAX_MEALS];
int numMeals = 0;

static void logLine(const char* line) {
    Serial.println(line);

    if (WiFi.status() != WL_CONNECTED) return;

    static unsigned long sent = 0;
    char packet[LOG_LINE_MAX + 24];
    int n = snprintf(packet, sizeof(packet), "%lu %s", sent + 1, line);
    if (n <= 0) return;
    if (n >= (int)sizeof(packet)) n = (int)sizeof(packet) - 1;

    if (!logUdp.beginPacket(WiFi.broadcastIP(), LOG_PORT)) return;
    logUdp.write((const uint8_t*)packet, (size_t)n);
    if (logUdp.endPacket()) sent++;
}

static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void logf(const char* fmt, ...) {
    char line[LOG_LINE_MAX];

    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    logLine(line);
}

static int insertMeal(const Meal& meal) {
    int at = numMeals;
    while (at > 0) {
        if (meals[at-1].timeOfDay == meal.timeOfDay) return -1;
        if (meals[at-1].timeOfDay < meal.timeOfDay) break;
        at--;
    }

    for (int i = numMeals; i > at; i--) {
        meals[i] = meals[i - 1];
    }

    meals[at] = meal;
    numMeals++;
    return at;
}

static void removeMeal(int index) {
    for (int i = index; i < numMeals - 1; i++) {
        meals[i] = meals[i + 1];
    }
    numMeals--;
}

static bool saveSchedule() {
    if (!prefs.putUChar(NVS_KEY_VERSION, SCHEDULE_FORMAT_VERSION)) {
        logLine("WARNING: could not write schedule version to NVS");
        return false;
    }

    if (numMeals == 0) {
        // putBytes rejects a zero-length blob, so an empty schedule is stored
        // as the absence of the key.
        prefs.remove(NVS_KEY_MEALS);
        return true;
    }

    StoredMeal stored[MAX_MEALS];
    uint8_t guessed = 0;
    for (int i = 0; i < numMeals; i++) {
        stored[i].portions = (int16_t)meals[i].portions;
        stored[i].timeOfDay = (int16_t)meals[i].timeOfDay;
        stored[i].lastFiredDay = (int32_t)meals[i].lastFiredDay;
        if (meals[i].stampGuessed) guessed |= 1 << i;
    }

    size_t bytes = (size_t)numMeals * sizeof(StoredMeal);
    if (prefs.putBytes(NVS_KEY_MEALS, stored, bytes) != bytes) {
        logLine("WARNING: could not persist meal schedule to NVS");
        return false;
    }

    // A separate key rather than a field, so the stored layout and every saved
    // schedule stay as they are.
    if (!prefs.putUChar(NVS_KEY_GUESSED, guessed)) {
        logLine("WARNING: could not persist guessed-clock flags to NVS");
        return false;
    }

    return true;
}

static void loadSchedule() {
    uint8_t version = prefs.getUChar(NVS_KEY_VERSION, 0);
    if (version != SCHEDULE_FORMAT_VERSION) {
        if (version != 0) {
            logf("Ignoring saved schedule in unknown format %u", version);
        }
        return;
    }

    size_t bytes = prefs.getBytesLength(NVS_KEY_MEALS);
    if (bytes == 0) return;

    if (bytes % sizeof(StoredMeal) != 0 || bytes > sizeof(StoredMeal) * MAX_MEALS) {
        logf("Ignoring saved schedule of implausible size %u", (unsigned)bytes);
        return;
    }

    StoredMeal stored[MAX_MEALS];
    if (prefs.getBytes(NVS_KEY_MEALS, stored, bytes) != bytes) {
        logLine("WARNING: could not read saved schedule from NVS");
        return;
    }

    uint8_t guessed = prefs.getUChar(NVS_KEY_GUESSED, 0);

    int count = (int)(bytes / sizeof(StoredMeal));
    for (int i = 0; i < count; i++) {
        if (stored[i].timeOfDay < 0 || stored[i].timeOfDay >= MINUTES_PER_DAY ||
            stored[i].portions < 1 || stored[i].portions > MAX_PORTIONS) {
            logf("Dropping out-of-range saved meal %d", i);
            continue;
        }

        Meal meal;
        meal.portions = stored[i].portions;
        meal.timeOfDay = stored[i].timeOfDay;
        meal.lastFiredDay = stored[i].lastFiredDay;
        meal.stampGuessed = (guessed >> i) & 1;

        if (meal.lastFiredDay < MEAL_NEVER_FIRED) meal.lastFiredDay = MEAL_NEVER_FIRED;

        if (insertMeal(meal) < 0) {
            logf("Ignoring duplicate meal in schedule");
        }
    }

    logf("Restored %d meal(s) from flash", numMeals);
}

bool clockSynced = false;

// Set when NTP has not answered within CLOCK_GUESS_AFTER_MS of boot. Meals are
// then served by guessBase plus the time since boot: the last time saved to
// flash, so late by however long the power was off, but at the right intervals.
bool clockGuessed = false;
time_t guessBase = 0;

long tzOffsetMinutes = 0;
time_t lastSyncEpoch = 0;

static bool clockKnown() {
    return clockSynced || clockGuessed;
}

static time_t uptimeSeconds() {
    return (time_t)(esp_timer_get_time() / 1000000);
}

// UTC as best the feeder knows it. Before NTP, the system clock only counts up
// from boot, so the guess is kept apart from it rather than written into it.
static time_t utcNow() {
    return clockSynced ? time(nullptr) : guessBase + uptimeSeconds();
}

static void refreshTzOffset() {
    time_t now = utcNow();

    struct tm local;
    struct tm utc;
    if (!localtime_r(&now, &local) || !gmtime_r(&now, &utc)) return;

    long minutes = (local.tm_hour * 60L + local.tm_min) - (utc.tm_hour * 60L + utc.tm_min);

    int days = local.tm_yday - utc.tm_yday;
    if (days == 1 || days < -1) {
        minutes += MINUTES_PER_DAY;
    } else if (days == -1 || days > 1) {
        minutes -= MINUTES_PER_DAY;
    }

    tzOffsetMinutes = minutes;
}

static time_t localNow() {
    return utcNow() + tzOffsetMinutes * 60;
}

static long localDay(time_t local) {
    return (long)(local / SECONDS_PER_DAY);
}

static int localMinuteOfDay(time_t local) {
    return (int)((local % SECONDS_PER_DAY) / 60);
}

static bool dispensingFromRequest = false;

static void motorRun() {
    digitalWrite(MOTOR_IN2_PIN, LOW);
    digitalWrite(MOTOR_IN1_PIN, HIGH);
}

static void motorStop() {
    digitalWrite(MOTOR_IN1_PIN, LOW);
    digitalWrite(MOTOR_IN2_PIN, LOW);
}

static bool waitForSensorLevel(int level, unsigned long deadline) {
    unsigned long stableSince = millis();

    // Signed difference, so millis() rolling over mid-meal is fine.
    while ((long)(millis() - deadline) < 0) {
        if (!dispensingFromRequest) server.handleClient();

        if (digitalRead(PORTION_SENSOR_PIN) != level) {
            stableSince = millis();
        } else if (millis() - stableSince >= PORTION_DEBOUNCE_MS) {
            return true;
        }
    }
    return false;
}

static bool dispenseOnePortion(unsigned long timeoutMs) {
    unsigned long deadline = millis() + timeoutMs;

    // The sensor may still be held by the portion that just passed (or by one
    // sitting there since the last meal), so wait for it to clear before
    // treating the next trip as a portion of its own.
    if (!waitForSensorLevel(PORTION_SENSOR_IDLE_LEVEL, deadline)) return false;

    return waitForSensorLevel(PORTION_SENSOR_ACTIVE_LEVEL, deadline);
}

static int dispensePortions(int portions, unsigned long timeoutMs) {
    int served = 0;

    motorRun();
    while (served < portions) {
        if (!dispenseOnePortion(timeoutMs)) {
            logf("Dispense: no portion within %lu ms (jam or empty hopper?)", timeoutMs);
            break;
        }

        served++;
        logf("Dispense: portion %d of %d", served, portions);
    }
    motorStop();

    return served;
}

static void MealEvent(const Meal& meal, int minutesLate) {
    if (minutesLate >= MEAL_LATE_MINUTES) {
        logf("MealEvent: serving %d portion(s) scheduled for %02d:%02d, %dh%02dm late"
             " (its time went by while the feeder was down)",
             meal.portions, meal.timeOfDay / 60, meal.timeOfDay % 60, minutesLate / 60,
             minutesLate % 60);
    } else {
        logf("MealEvent: serving %d portion(s) (scheduled for %02d:%02d)", meal.portions,
             meal.timeOfDay / 60, meal.timeOfDay % 60);
    }

    int served = dispensePortions(meal.portions, PORTION_TIMEOUT_MS);

    logf("MealEvent: served %d of %d portion(s)", served, meal.portions);
}

static void serviceSchedule() {
    if (!clockKnown()) return;

    time_t local = localNow();
    long today = localDay(local);
    int minute = localMinuteOfDay(local);

    for (int i = 0; i < numMeals; i++) {
        // Tomorrow is allowed: a meal served on a guessed clock can stand in for
        // tomorrow's (see settleGuessedMeals()), and a guessed clock restored
        // from flash can trail a meal stamped just after it was last saved.
        if (meals[i].lastFiredDay > today + 1) {
            logf("Meal %d is settled for a day in the future; forgetting that", i);
            meals[i].lastFiredDay = MEAL_NEVER_FIRED;
            meals[i].stampGuessed = false;
        }

        if (minute < meals[i].timeOfDay) continue;

        if (!(meals[i].lastFiredDay < today)) continue;

        meals[i].lastFiredDay = today;
        meals[i].stampGuessed = !clockSynced;
        MealEvent(meals[i], minute - meals[i].timeOfDay);

        if (!saveSchedule()) {
            logLine("WARNING: fed, but could not record it to flash;"
                    " a restart today may feed this meal again");
        }
    }
}

static const char* methodName(HTTPMethod method) {
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_PUT: return "PUT";
        case HTTP_DELETE: return "DELETE";
        default: return "?";
    }
}

static void sendJson(int code, const JsonDocument& doc) {
    logf("%s %s %s -> %d", server.client().remoteIP().toString().c_str(),
         methodName(server.method()), server.uri().c_str(), code);

    String body;
    serializeJson(doc, body);
    server.send(code, "application/json", body);
}

static void sendError(int code, const char* message) {
    JsonDocument doc;
    doc["error"] = message;
    sendJson(code, doc);
}

static void mealToJson(const Meal& meal, JsonObject obj) {
    obj["timeOfDay"] = meal.timeOfDay;
    obj["portions"] = meal.portions;

    if (!clockKnown()) return;

    obj["fedToday"] = meal.lastFiredDay == localDay(localNow());
}

static void clockToJson(JsonObject obj) {
    obj["synced"] = clockSynced;
    obj["guessed"] = clockGuessed;
    obj["tzOffsetMinutes"] = tzOffsetMinutes;
    obj["tz"] = POSIX_TZ;

    if (!clockKnown()) {
        long left = (long)CLOCK_GUESS_AFTER_MS - (long)millis();
        obj["guessInSeconds"] = left > 0 ? left / 1000 : 0;
        return;
    }

    time_t now = utcNow();
    time_t local = localNow();

    obj["epoch"] = (long long)now;
    obj["localMinuteOfDay"] = localMinuteOfDay(local);

    if (!clockSynced) return;

    long age = (long)(now - lastSyncEpoch);
    obj["secondsSinceSync"] = age;
    obj["stale"] = age > CLOCK_STALE_SECONDS;
}

static bool intArg(const char* name, int& out) {
    if (!server.hasArg(name)) return false;

    String raw = server.arg(name);
    int value = raw.toInt();
    if (value == 0 && raw != "0") return false;

    out = value;
    return true;
}

static bool readBody(JsonDocument& doc) {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing request body (JSON needs Content-Type: application/json)");
        return false;
    }

    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        logf("JSON parse error: %s", err.c_str());
        sendError(400, "Body is not valid JSON");
        return false;
    }

    return true;
}

// A meal created after its own time today is skipped rather than served: it did
// not exist when its slot went by, so there is nothing to catch up on. Stamping
// it with today settles it until tomorrow.
static void skipIfTimePassed(Meal& meal) {
    if (!clockKnown()) return;

    time_t local = localNow();
    if (localMinuteOfDay(local) >= meal.timeOfDay) {
        meal.lastFiredDay = localDay(local);
        meal.stampGuessed = !clockSynced;
    }
}

static bool mealFromJson(JsonObjectConst obj, Meal& meal) {
    if (!obj["timeOfDay"].is<int>() || !obj["portions"].is<int>()) {
        sendError(400, "Expected integer fields 'timeOfDay' and 'portions'");
        return false;
    }

    int timeOfDay = obj["timeOfDay"];
    int portions = obj["portions"];

    if (timeOfDay < 0 || timeOfDay >= MINUTES_PER_DAY) {
        sendError(400, "'timeOfDay' must be between 0 and 1439");
        return false;
    }

    if (portions < 1 || portions > MAX_PORTIONS) {
        sendError(400, "'portions' must be between 1 and 10");
        return false;
    }

    meal.timeOfDay = timeOfDay;
    meal.portions = portions;
    meal.lastFiredDay = MEAL_NEVER_FIRED;
    meal.stampGuessed = false;
    return true;
}

static int mealIndexAtTime(int timeOfDay) {
    for (int i = 0; i < numMeals; i++) {
        if (meals[i].timeOfDay == timeOfDay) return i;
    }

    return -1;
}

// Parses a meal time (HH:MM) from the URL. Returns -1 (and answers the request)
// when it is malformed or no meal is scheduled then.
static int mealIndexFromUri() {
    String raw = server.pathArg(0);
    int hours = 0;
    int minutes = 0;
    int used = 0;

    if (sscanf(raw.c_str(), "%d:%d%n", &hours, &minutes, &used) != 2 ||
        used != (int)raw.length() || hours < 0 || hours > 23 || minutes < 0 ||
        minutes > 59) {
        sendError(400, "Expected a meal time as HH:MM, e.g. /api/meals/07:30");
        return -1;
    }

    int index = mealIndexAtTime(hours * 60 + minutes);
    if (index < 0) {
        char message[48];
        snprintf(message, sizeof(message), "No meal scheduled at %02d:%02d", hours,
                 minutes);
        sendError(404, message);
        return -1;
    }

    return index;
}

// GET /api/settings -> get meals and clock sync information
void handleGetSettings() {
    JsonDocument doc;
    JsonArray arr = doc["meals"].to<JsonArray>();

    for (int i = 0; i < numMeals; i++) {
        mealToJson(meals[i], arr.add<JsonObject>());
    }

    clockToJson(doc["clock"].to<JsonObject>());
    sendJson(200, doc);
}

// GET /api/meals -> every meal the feeder is scheduled to serve.
void handleGetMeals() {
    JsonDocument doc;
    JsonArray arr = doc["meals"].to<JsonArray>();

    for (int i = 0; i < numMeals; i++) {
        mealToJson(meals[i], arr.add<JsonObject>());
    }

    sendJson(200, doc);
}

// POST /api/meals -> append a meal.
void handleAddMeal() {
    if (numMeals >= MAX_MEALS) {
        sendError(409, "Meal schedule is full");
        return;
    }

    JsonDocument doc;
    if (!readBody(doc)) return;

    Meal meal;
    if (!mealFromJson(doc.as<JsonObjectConst>(), meal)) return;

    skipIfTimePassed(meal);

    int index = insertMeal(meal);

    if (index < 0) {
        logf("Found existing meal at %02d:%02d. Skipping duplicate.", meal.timeOfDay / 60, meal.timeOfDay % 60);
        char message[48];
        snprintf(message, sizeof(message), "Meal already scheduled at %02d:%02d",
                 meal.timeOfDay / 60, meal.timeOfDay % 60);
        sendError(400, message);
        return;
    }

    bool persisted = saveSchedule();
    logf("Added meal %d: %d portions at %d", index, meal.portions, meal.timeOfDay);

    JsonDocument res;
    res["index"] = index;
    res["persisted"] = persisted;
    mealToJson(meal, res["meal"].to<JsonObject>());
    sendJson(201, res);
}

// PUT /api/meals -> modify a meal.
void handleModifyMeal() {
    JsonDocument doc;
    if (!readBody(doc)) return;

    Meal wanted;
    if (!mealFromJson(doc.as<JsonObjectConst>(), wanted)) return;

    int index = mealIndexAtTime(wanted.timeOfDay);
    if (index < 0) {
        char message[48];
        snprintf(message, sizeof(message), "No meal scheduled at %02d:%02d",
                 wanted.timeOfDay / 60, wanted.timeOfDay % 60);
        sendError(404, message);
        return;
    }

    int before = meals[index].portions;
    meals[index].portions = wanted.portions;

    bool persisted = saveSchedule();
    logf("Modified meal %d at %02d:%02d: %d portion(s), was %d", index,
         meals[index].timeOfDay / 60, meals[index].timeOfDay % 60, meals[index].portions,
         before);

    JsonDocument res;
    res["index"] = index;
    res["persisted"] = persisted;
    res["previousPortions"] = before;
    mealToJson(meals[index], res["meal"].to<JsonObject>());
    sendJson(200, res);
}

// DELETE /api/meals/<HH:MM> -> delete the meal at that time.
void handleDeleteMeal() {
    int index = mealIndexFromUri();
    if (index < 0) return;

    Meal removed = meals[index];
    removeMeal(index);

    bool persisted = saveSchedule();
    logf("Deleted meal at %02d:%02d", removed.timeOfDay / 60, removed.timeOfDay % 60);

    JsonDocument res;
    res["persisted"] = persisted;
    mealToJson(removed, res["meal"].to<JsonObject>());
    sendJson(200, res);
}

static void saveClock() {
    prefs.putLong64(NVS_KEY_CLOCK, (int64_t)utcNow());
}

// Moves every day stamped on the guessed clock onto the real one, now that we
// know how far out the guess was. Each meal served on the guess stands in for
// whichever of its real slots it fell closest to, so the cat is never fed the
// same meal twice within twelve hours: a breakfast served at 01:30 real time is
// today's breakfast and 07:30 is skipped; one served at 21:30 is tomorrow's.
static void settleGuessedMeals(time_t jumpSeconds) {
    long shift = (long)((jumpSeconds >= 0 ? jumpSeconds + SECONDS_PER_DAY / 2
                                          : jumpSeconds - SECONDS_PER_DAY / 2) /
                        SECONDS_PER_DAY);
    int moved = 0;

    for (int i = 0; i < numMeals; i++) {
        if (!meals[i].stampGuessed) continue;

        meals[i].stampGuessed = false;
        if (meals[i].lastFiredDay == MEAL_NEVER_FIRED) continue;

        meals[i].lastFiredDay += shift;
        moved++;
    }

    logf("The guessed clock was %+ld minute(s) out; moved %d meal record(s) by %+ld day(s)",
         (long)(jumpSeconds / 60), moved, shift);

    if (moved > 0 && !saveSchedule()) {
        logLine("WARNING: could not record settled meals to flash;"
                " a restart today may feed them again");
    }
}

static void applyTime(long long epoch, const char* source) {
    bool wasGuessed = clockGuessed;
    time_t guessedLocal = localNow();

    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    lastSyncEpoch = (time_t)epoch;
    clockSynced = true;
    clockGuessed = false;
    refreshTzOffset();  // the zone rule, now that there is an instant to apply it to

    time_t local = localNow();
    logf("Clock synced from %s: local time %02d:%02d (UTC%+ld:%02ld)", source,
         localMinuteOfDay(local) / 60, localMinuteOfDay(local) % 60,
         tzOffsetMinutes / 60, labs(tzOffsetMinutes) % 60);

    if (wasGuessed) settleGuessedMeals(local - guessedLocal);
    saveClock();
}

// Falls back to the guessed clock once NTP has had long enough, and keeps the
// last known time in flash for the next boot to guess from.
static void serviceClock() {
    static unsigned long lastSave = 0;

    if (!clockKnown() && millis() >= CLOCK_GUESS_AFTER_MS) {
        clockGuessed = true;
        refreshTzOffset();

        time_t local = localNow();
        logf("No NTP after %lu min; feeding by a guessed clock, local %02d:%02d."
             " Still asking NTP.",
             CLOCK_GUESS_AFTER_MS / 60000, localMinuteOfDay(local) / 60,
             localMinuteOfDay(local) % 60);
    }

    // Unsigned arithmetic, so millis() rollover is fine.
    unsigned long now = millis();
    if (now - lastSave < CLOCK_SAVE_INTERVAL_MS) return;

    lastSave = now;
    saveClock();
}

static void serviceNtp() {
    static bool started = false;
    static unsigned long lastCheck = 0;

    // DNS has nowhere to go until we are associated, and SNTP needs a name.
    if (WiFi.status() != WL_CONNECTED) return;

    if (!started) {
        started = true;
        sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
        configTzTime(POSIX_TZ, NTP_SERVER_1, NTP_SERVER_2);
        logf("Asking %s for the time, zone %s", NTP_SERVER_1, POSIX_TZ);
    }

    // Unsigned arithmetic, so millis() rollover is fine.
    unsigned long now = millis();
    if (now - lastCheck < NTP_CHECK_INTERVAL_MS) return;
    lastCheck = now;

    time_t epoch = time(nullptr);
    if (epoch < MIN_VALID_EPOCH) return;  // nothing has answered yet

    if (!clockSynced) {
        applyTime(epoch, "NTP");
        return;
    }

    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) lastSyncEpoch = epoch;
}

// POST /api/dispense -> serve portions now, ignoring the schedule and clock
void handleDispense() {
    int portions = 1;

    if (server.hasArg("portions")) {
        if (!intArg("portions", portions)) {
            sendError(400, "'portions' must be an integer");
            return;
        }
        if (portions < 1 || portions > MAX_PORTIONS) {
            sendError(400, "'portions' must be between 1 and 10");
            return;
        }
    } else if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument doc;
        if (!readBody(doc)) return;

        if (!doc["portions"].isNull()) {
            if (!doc["portions"].is<int>()) {
                sendError(400, "'portions' must be an integer");
                return;
            }
            portions = doc["portions"];
            if (portions < 1 || portions > MAX_PORTIONS) {
                sendError(400, "'portions' must be between 1 and 10");
                return;
            }
        }
    }

    int timeoutMs = PORTION_TIMEOUT_MS;
    if (server.hasArg("timeout")) {
        if (!intArg("timeout", timeoutMs) || timeoutMs < 100 ||
            timeoutMs > PORTION_TIMEOUT_MAX_MS) {
            sendError(400, "'timeout' must be between 100 and 60000 ms");
            return;
        }
    }

    logf("Manual dispense of %d portion(s) requested, %d ms per portion", portions, timeoutMs);

    dispensingFromRequest = true;
    int served = dispensePortions(portions, (unsigned long)timeoutMs);
    dispensingFromRequest = false;

    JsonDocument res;
    res["requested"] = portions;
    res["served"] = served;
    res["complete"] = served == portions;
    res["timeoutMs"] = timeoutMs;
    sendJson(200, res);
}

void handleNotFound() {
    sendError(404, "No such endpoint");
}

static bool otaReady = false;

// Over-the-air updates, so a change does not need the board on a cable. Started
// on the first connection rather than from setup(), which no longer waits for
// WiFi, and only once -- a reconnect must not register the handlers again.
static void startOta() {
    if (otaReady) return;

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        // An update landing between portions must not leave the auger turning
        // through the write and the reboot that follows it.
        motorStop();
        logLine("OTA: update starting, the feeder will reboot when it lands");
    });

    ArduinoOTA.onEnd([]() { logLine("OTA: update written, rebooting"); });

    ArduinoOTA.onError([](ota_error_t error) {
        // Nothing has been committed at this point: otadata still points at the
        // partition we are running from, so a failure costs nothing but a retry.
        logf("OTA: update failed (error %u), still running the old firmware",
             (unsigned)error);
    });

    ArduinoOTA.begin();
    otaReady = true;
    logf("OTA updates enabled as %s.local", OTA_HOSTNAME);
}

// Keeps WiFi coming back without ever waiting for it. Called from loop() on
// every pass, including the very first: setup() only asks for a connection, and
// this is what notices it arrived.
static void serviceWifi() {
    static bool wasConnected = false;
    static unsigned long lastAttempt = 0;
    static unsigned long lostAt = 0;

    if (WiFi.status() == WL_CONNECTED) {
        if (!wasConnected) {
            wasConnected = true;
            // First line out after an outage, and the first the LAN hears at
            // all: logLine() only broadcasts while connected, so the matching
            // "lost" line above reached the serial cable alone.
            logf("WiFi connected after %lus, IP: %s", (millis() - lostAt) / 1000,
                 WiFi.localIP().toString().c_str());
            startOta();
        }
        return;
    }

    if (wasConnected) {
        wasConnected = false;
        lostAt = millis();
        logLine("WiFi connection lost; retrying in the background, meals continue");
    }

    // Unsigned arithmetic, so millis() rollover is fine.
    unsigned long now = millis();
    if (now - lastAttempt < WIFI_RETRY_INTERVAL_MS) return;

    lastAttempt = now;
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void setup() {
    Serial.begin(115200);

    // Before anything that can block: a floating IN1/IN2 must not leave the
    // auger turning while we sit in the WiFi connect loop.
    pinMode(MOTOR_IN1_PIN, OUTPUT);
    pinMode(MOTOR_IN2_PIN, OUTPUT);
    motorStop();
    pinMode(PORTION_SENSOR_PIN, INPUT_PULLUP);

    if (prefs.begin(NVS_NAMESPACE, false)) {
        loadSchedule();

        // Where a guessed clock starts, should NTP not answer: the last time
        // this feeder knew, behind by however long the power was off.
        guessBase = (time_t)prefs.getLong64(NVS_KEY_CLOCK, 0) - uptimeSeconds();
    } else {
        logLine("WARNING: could not open NVS; schedule will not persist");
    }

    // The zone rule is ours before any clock is: whenever an epoch does arrive,
    // local time is right immediately, NTP or not.
    setenv("TZ", POSIX_TZ, 1);
    tzset();

    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    logLine("Connecting to WiFi in the background");

    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/meals", HTTP_GET, handleGetMeals);
    server.on("/api/meals", HTTP_POST, handleAddMeal);
    server.on("/api/meals", HTTP_PUT, handleModifyMeal);
    server.on(UriBraces("/api/meals/{}"), HTTP_DELETE, handleDeleteMeal);
    server.on("/api/dispense", HTTP_POST, handleDispense);
    server.onNotFound(handleNotFound);

    server.begin();
    logf("Listening for HTTP on port %d", HTTP_PORT);

    logf("Streaming logs to UDP port %d", LOG_PORT);
    logf("Clock not set: meals wait for NTP, or %lu min until the feeder guesses",
         CLOCK_GUESS_AFTER_MS / 60000);
}

void loop() {

    serviceWifi();

    server.handleClient();
    if (otaReady) ArduinoOTA.handle();
    serviceNtp();
    serviceClock();

    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck >= 1000) {  // unsigned arithmetic, so millis() rollover is fine
        lastCheck = now;
        refreshTzOffset();  // cheap, and a switchover must not wait on the network
        serviceSchedule();
    }
}
