#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
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

// The clock has no battery, so it only means anything once the laptop has sent
// us the time. Anything before 2020 is a device that has not been told yet.
#define MIN_VALID_EPOCH 1577836800LL  // 2020-01-01T00:00:00Z
#define MAX_TZ_OFFSET_MINUTES 840     // +14:00 .. -14:00 covers every real zone
#define CLOCK_STALE_SECONDS (14L * SECONDS_PER_DAY)

// The laptop has no fixed address, and it is the laptop that moves, so the
// feeder broadcasts instead of being told where to look: a sync request goes to
// whoever is listening on the LAN, and the reply comes back to this socket.
#define TIME_SYNC_PORT 3956
#define TIME_SYNC_INTERVAL_MS 30000
#define TIME_SYNC_PACKET_MAX 192

// Log lines are broadcast too, so a viewer can come and go without the feeder
// knowing or caring, and a missing listener can never block the scheduler.
#define LOG_PORT 3957
#define LOG_LINE_MAX 200

#define MEAL_NEVER_FIRED (-1L)

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
#define SCHEDULE_FORMAT_VERSION 1

typedef struct {

    int portions;
    int timeOfDay;    // minutes since local midnight
    long lastFiredDay;  // local day number we last fed this meal on, or MEAL_NEVER_FIRED

} Meal;

// What actually goes to flash. Only the schedule itself: which meals were
// already served is runtime state, and armSchedule() rebuilds it at sync time.
typedef struct {

    int16_t portions;
    int16_t timeOfDay;

} StoredMeal;

WebServer server(HTTP_PORT);
Preferences prefs;

WiFiUDP syncUdp;  // bound: sends sync requests and receives the replies
WiFiUDP logUdp;   // send only: nothing ever answers a log line

Meal meals[MAX_MEALS];
int numMeals = 0;

// Everything the feeder has to say goes through here: to the serial monitor as
// before, and to anyone listening on the LAN for the live stream. Logging must
// never fail a caller, so a line that cannot be broadcast is simply a line that
// only reached the cable.
static void logLine(const char* line) {
    Serial.println(line);

    if (WiFi.status() != WL_CONNECTED) return;

    // Numbering the lines lets a viewer see what UDP dropped, which is the price
    // of a transport that never blocks the feeder waiting on a listener. The
    // counter only advances for a line that went out, so a gap means a loss
    // rather than a line logged before the network was up.
    static unsigned long sent = 0;
    char packet[LOG_LINE_MAX + 24];
    int n = snprintf(packet, sizeof(packet), "%lu %s", sent + 1, line);
    if (n <= 0) return;
    if (n >= (int)sizeof(packet)) n = (int)sizeof(packet) - 1;

    if (!logUdp.beginPacket(WiFi.broadcastIP(), LOG_PORT)) return;
    logUdp.write((const uint8_t*)packet, (size_t)n);
    if (logUdp.endPacket()) sent++;
}

// The format attribute is what makes the compiler check these call sites the
// way it checks printf's.
static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void logf(const char* fmt, ...) {
    char line[LOG_LINE_MAX];

    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    logLine(line);
}

// The schedule is kept in time order, so it reads like a timetable and the
// indices the API hands out follow the clock. Inserting into place is enough to
// keep it that way, and walking back over equal times leaves meals that share a
// time in the order they were added. Returns where the meal landed.
static int insertMeal(const Meal& meal) {
    int at = numMeals;
    while (at > 0 && meals[at - 1].timeOfDay > meal.timeOfDay) at--;

    for (int i = numMeals; i > at; i--) {
        meals[i] = meals[i - 1];
    }

    meals[at] = meal;
    numMeals++;
    return at;
}

// Drops a meal, closing the gap so the order is preserved and no stale copy is
// left past the end of the list.
static void removeMeal(int index) {
    for (int i = index; i < numMeals - 1; i++) {
        meals[i] = meals[i + 1];
    }
    numMeals--;
}

// Writes the schedule to flash. Returns false if it did not stick, so the
// caller can tell the client its change will not survive a reboot.
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
    for (int i = 0; i < numMeals; i++) {
        stored[i].portions = (int16_t)meals[i].portions;
        stored[i].timeOfDay = (int16_t)meals[i].timeOfDay;
    }

    size_t bytes = (size_t)numMeals * sizeof(StoredMeal);
    if (prefs.putBytes(NVS_KEY_MEALS, stored, bytes) != bytes) {
        logLine("WARNING: could not persist meal schedule to NVS");
        return false;
    }

    return true;
}

// Restores the schedule saved by saveSchedule(). Anything that does not look
// like a schedule we wrote is discarded rather than trusted.
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
        meal.lastFiredDay = MEAL_NEVER_FIRED;
        insertMeal(meal);  // a schedule saved before meals were ordered
    }

    logf("Restored %d meal(s) from flash", numMeals);
}

// Set by POST /api/time. Until then the scheduler stays parked.
bool clockSynced = false;
long tzOffsetMinutes = 0;
time_t lastSyncEpoch = 0;

// Local wall-clock seconds: UTC as the laptop gave it, shifted into the
// laptop's timezone. We never use the libc timezone, so this stays honest.
static time_t localNow() {
    return time(nullptr) + tzOffsetMinutes * 60;
}

static long localDay(time_t local) {
    return (long)(local / SECONDS_PER_DAY);
}

static int localMinuteOfDay(time_t local) {
    return (int)((local % SECONDS_PER_DAY) / 60);
}

// serviceSchedule() dispenses from loop(), where pumping the web server keeps
// the API answerable through a long meal. /api/dispense dispenses from inside a
// request, where re-entering handleClient() would trample the request we have
// not answered yet.
static bool dispensingFromRequest = false;

static void motorRun() {
    digitalWrite(MOTOR_IN2_PIN, LOW);
    digitalWrite(MOTOR_IN1_PIN, HIGH);
}

static void motorStop() {
    digitalWrite(MOTOR_IN1_PIN, LOW);
    digitalWrite(MOTOR_IN2_PIN, LOW);
}

// Blocks until the sensor has read `level` steadily for the debounce window,
// or until `deadline` passes. Dispensing a meal takes seconds, so we keep
// answering HTTP while we wait instead of going deaf for the whole meal.
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

// Waits for one portion to go past the sensor, with the motor already running.
// Returns false if nothing arrived before the timeout.
static bool dispenseOnePortion(unsigned long timeoutMs) {
    unsigned long deadline = millis() + timeoutMs;

    // The sensor may still be held by the portion that just passed (or by one
    // sitting there since the last meal), so wait for it to clear before
    // treating the next trip as a portion of its own.
    if (!waitForSensorLevel(PORTION_SENSOR_IDLE_LEVEL, deadline)) return false;

    return waitForSensorLevel(PORTION_SENSOR_ACTIVE_LEVEL, deadline);
}

// Runs the auger until the sensor has counted out `portions` portions. Returns
// how many actually arrived, which is short of what was asked on a timeout.
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

static void MealEvent(const Meal& meal) {
    logf("MealEvent: serving %d portion(s) (scheduled for %02d:%02d)", meal.portions,
         meal.timeOfDay / 60, meal.timeOfDay % 60);

    int served = dispensePortions(meal.portions, PORTION_TIMEOUT_MS);

    logf("MealEvent: served %d of %d portion(s)", served, meal.portions);
}

// Marks meals whose time has already passed today as done, so that a device
// that boots (or gets its clock) in the evening does not immediately dump the
// whole day's meals into the bowl at once.
static void armSchedule() {
    if (!clockSynced) return;

    time_t local = localNow();
    long today = localDay(local);
    int minute = localMinuteOfDay(local);

    for (int i = 0; i < numMeals; i++) {
        if (meals[i].lastFiredDay == MEAL_NEVER_FIRED && minute >= meals[i].timeOfDay) {
            meals[i].lastFiredDay = today;
        }
    }
}

// Feeds any meal whose time arrived since the last check. Called once a second
// from loop(); a meal fires at most once per local day.
static void serviceSchedule() {
    if (!clockSynced) return;

    time_t local = localNow();
    long today = localDay(local);
    int minute = localMinuteOfDay(local);

    for (int i = 0; i < numMeals; i++) {
        // `<` rather than `!=` so a clock correction that moves us backwards
        // cannot make a meal fire twice.
        if (meals[i].lastFiredDay < today && minute >= meals[i].timeOfDay) {
            meals[i].lastFiredDay = today;
            MealEvent(meals[i]);
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

// Every answer the feeder gives leaves through here, so this is also where each
// request is logged: one line with who asked, what they asked for, and what
// they got. Nothing has to remember to log itself, and a request refused before
// a handler could do anything is on the log like any other.
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
}

static void clockToJson(JsonObject obj) {
    obj["synced"] = clockSynced;
    obj["tzOffsetMinutes"] = tzOffsetMinutes;

    if (!clockSynced) return;

    time_t now = time(nullptr);
    time_t local = localNow();
    long age = (long)(now - lastSyncEpoch);

    obj["epoch"] = (long long)now;
    obj["localMinuteOfDay"] = localMinuteOfDay(local);
    obj["secondsSinceSync"] = age;
    obj["stale"] = age > CLOCK_STALE_SECONDS;
}

// Query arguments survive on every content type, so /api/dispense takes them as
// well as a JSON body: `POST /api/dispense?portions=2` needs no header and no
// quoting, which is what you want from a shell or a browser bar.
static bool intArg(const char* name, int& out) {
    if (!server.hasArg(name)) return false;

    String raw = server.arg(name);
    int value = raw.toInt();
    if (value == 0 && raw != "0") return false;

    out = value;
    return true;
}

// Reads the request body as JSON. Returns false (and answers the request) when
// the body is missing or malformed.
static bool readBody(JsonDocument& doc) {
    if (!server.hasArg("plain")) {
        // A JSON body only reaches us as "plain" when the request does not
        // claim to be a form: WebServer appends a form body to the argument
        // string, where _parseArguments() drops any token with no '=' in it.
        // So `curl -d '{...}'` without a Content-Type header arrives as
        // nothing at all, and the message has to say why.
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

// Pulls a meal out of a JSON object. Returns false (and answers the request)
// when a field is missing or out of range.
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
    return true;
}

// Parses a meal index from the URL. Returns -1 (and answers the request) when
// it does not name an existing meal.
static int mealIndexFromUri() {
    String raw = server.pathArg(0);
    int index = raw.toInt();

    if (index < 0 || index >= numMeals || (index == 0 && raw != "0")) {
        sendError(404, "No such meal");
        return -1;
    }

    return index;
}

// GET /api/settings -> every meal the feeder is scheduled to serve.
void handleGetSettings() {
    JsonDocument doc;
    JsonArray arr = doc["meals"].to<JsonArray>();

    for (int i = 0; i < numMeals; i++) {
        mealToJson(meals[i], arr.add<JsonObject>());
    }

    clockToJson(doc["clock"].to<JsonObject>());
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

    int index = insertMeal(meal);
    armSchedule();  // don't serve a meal whose time passed before it existed
    bool persisted = saveSchedule();
    logf("Added meal %d: %d portions at %d", index, meal.portions, meal.timeOfDay);

    JsonDocument res;
    res["index"] = index;
    res["persisted"] = persisted;
    mealToJson(meal, res["meal"].to<JsonObject>());
    sendJson(201, res);
}

// PUT /api/meals/<index> -> replace a meal.
void handleChangeMeal() {
    int index = mealIndexFromUri();
    if (index < 0) return;

    JsonDocument doc;
    if (!readBody(doc)) return;

    Meal meal;
    if (!mealFromJson(doc.as<JsonObjectConst>(), meal)) return;

    // A meal already served today stays served, so moving its time later in the
    // day does not feed the cat a second time.
    if (meals[index].lastFiredDay != MEAL_NEVER_FIRED) {
        meal.lastFiredDay = meals[index].lastFiredDay;
    }

    // A new time belongs somewhere else in the schedule, so the meal moves and
    // the index in the response is not necessarily the one that was asked for.
    removeMeal(index);
    int newIndex = insertMeal(meal);

    armSchedule();
    bool persisted = saveSchedule();
    logf("Changed meal %d: %d portions at %d (now meal %d)", index, meal.portions,
         meal.timeOfDay, newIndex);

    JsonDocument res;
    res["index"] = newIndex;
    res["persisted"] = persisted;
    mealToJson(meal, res["meal"].to<JsonObject>());
    sendJson(200, res);
}

// DELETE /api/meals/<index> -> drop a meal, closing the gap it leaves.
void handleDeleteMeal() {
    int index = mealIndexFromUri();
    if (index < 0) return;

    Meal removed = meals[index];
    removeMeal(index);

    bool persisted = saveSchedule();
    logf("Deleted meal %d", index);

    JsonDocument res;
    res["persisted"] = persisted;
    mealToJson(removed, res["meal"].to<JsonObject>());
    sendJson(200, res);
}

// GET /api/time -> what the feeder thinks the time is.
void handleGetTime() {
    JsonDocument doc;
    clockToJson(doc.to<JsonObject>());
    sendJson(200, doc);
}

// POST /api/time -> set the clock from the laptop. The device has no RTC
// battery and no route to the internet, so this is the only way it learns the
// time; tools/sync-time.sh runs this weekly.
// Reads a time out of a pushed request body or a sync reply. Returns nullptr
// when it is usable, or why it was refused, so the HTTP path can answer with
// the reason and the sync path can log it.
static const char* timeFromJson(JsonObjectConst obj, long long& epoch, long& offset) {
    if (!obj["epoch"].is<long long>()) {
        return "Expected integer field 'epoch' (seconds since 1970, UTC)";
    }

    epoch = obj["epoch"];
    if (epoch < MIN_VALID_EPOCH) return "'epoch' is implausibly far in the past";

    offset = 0;
    if (!obj["tzOffsetMinutes"].isNull()) {
        if (!obj["tzOffsetMinutes"].is<int>()) return "'tzOffsetMinutes' must be an integer";

        offset = (long)obj["tzOffsetMinutes"].as<int>();
        if (offset < -MAX_TZ_OFFSET_MINUTES || offset > MAX_TZ_OFFSET_MINUTES) {
            return "'tzOffsetMinutes' must be between -840 and 840";
        }
    }

    return nullptr;
}

// The one place the clock is set, whichever way the time arrived. `source` only
// shapes the log line.
static void applyTime(long long epoch, long offset, const char* source) {
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    tzOffsetMinutes = offset;
    lastSyncEpoch = (time_t)epoch;
    clockSynced = true;
    armSchedule();

    time_t local = localNow();
    logf("Clock synced from %s: local time %02d:%02d (UTC%+ld:%02ld)", source,
         localMinuteOfDay(local) / 60, localMinuteOfDay(local) % 60, offset / 60,
         labs(offset) % 60);
}

// Asks the LAN for the time. Broadcast, because the feeder does not know the
// laptop's address and the laptop's address changes; whoever is running the
// responder answers, and anything else on the network ignores it.
static void requestTimeSync() {
    if (WiFi.status() != WL_CONNECTED) return;

    static const char request[] = "{\"catfeeder\":\"sync-request\"}";
    if (!syncUdp.beginPacket(WiFi.broadcastIP(), TIME_SYNC_PORT)) return;

    syncUdp.write((const uint8_t*)request, sizeof(request) - 1);
    syncUdp.endPacket();
}

// Applies a sync reply, and asks again every TIME_SYNC_INTERVAL_MS for as long
// as the clock is unset. Called from loop().
static void serviceTimeSync() {
    static unsigned long lastRequest = 0;
    static bool asked = false;

    // Drain whatever arrived, even once synced: an unread socket only fills up.
    for (int size = syncUdp.parsePacket(); size > 0; size = syncUdp.parsePacket()) {
        char packet[TIME_SYNC_PACKET_MAX];
        int n = syncUdp.read((uint8_t*)packet, sizeof(packet) - 1);
        if (n <= 0) continue;
        packet[n] = '\0';

        IPAddress from = syncUdp.remoteIP();

        // Once the clock is set, nothing on the LAN gets to move it again; a push
        // to /api/time is the deliberate way to correct it. Say so rather than
        // dropping it in silence, or an answer that arrives too late looks
        // exactly like logging that does not work.
        if (clockSynced) {
            logf("Ignoring sync reply from %s: the clock is already set", from.toString().c_str());
            continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, packet)) {
            logf("Ignoring unreadable sync reply from %s", from.toString().c_str());
            continue;
        }

        // Our own broadcast, on a network that echoes it back to us.
        if (doc["catfeeder"] == "sync-request") continue;

        long long epoch = 0;
        long offset = 0;
        const char* why = timeFromJson(doc.as<JsonObjectConst>(), epoch, offset);
        if (why) {
            logf("Ignoring sync reply from %s: %s", from.toString().c_str(), why);
            continue;
        }

        applyTime(epoch, offset, from.toString().c_str());
    }

    if (clockSynced) return;

    // Unsigned arithmetic, so millis() rollover is fine.
    unsigned long now = millis();
    if (asked && now - lastRequest < TIME_SYNC_INTERVAL_MS) return;

    lastRequest = now;
    asked = true;
    logf("Clock not set: asking the network for the time on UDP port %d", TIME_SYNC_PORT);
    requestTimeSync();
}

void handleSetTime() {
    JsonDocument doc;
    if (!readBody(doc)) return;

    long long epoch = 0;
    long offset = 0;
    const char* why = timeFromJson(doc.as<JsonObjectConst>(), epoch, offset);
    if (why) {
        sendError(400, why);
        return;
    }

    applyTime(epoch, offset, "a pushed request");

    JsonDocument res;
    clockToJson(res.to<JsonObject>());
    sendJson(200, res);
}

// POST /api/dispense -> serve portions now, ignoring the schedule and the
// clock. Body is optional; {"portions": n} defaults to one portion.
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
    } else {
        logLine("WARNING: could not open NVS; schedule will not persist");
    }

    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    logf("Connected, IP: %s", WiFi.localIP().toString().c_str());

    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/meals", HTTP_GET, handleGetSettings);
    server.on("/api/meals", HTTP_POST, handleAddMeal);
    server.on(UriBraces("/api/meals/{}"), HTTP_PUT, handleChangeMeal);
    server.on(UriBraces("/api/meals/{}"), HTTP_DELETE, handleDeleteMeal);
    server.on("/api/time", HTTP_GET, handleGetTime);
    server.on("/api/time", HTTP_POST, handleSetTime);
    server.on("/api/dispense", HTTP_POST, handleDispense);
    server.onNotFound(handleNotFound);

    server.begin();
    logf("Listening for HTTP on port %d", HTTP_PORT);

    // Bound before the first request so the reply has somewhere to land.
    syncUdp.begin(TIME_SYNC_PORT);
    logf("Streaming logs to UDP port %d", LOG_PORT);
    logLine("Clock not set: no meal will be served until it is");
    serviceTimeSync();
}

void loop() {

    if (WiFi.status() != WL_CONNECTED) {
        logLine("WiFi connection lost! Reconnecting...");
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        logf("Connected, IP: %s", WiFi.localIP().toString().c_str());
    }

    server.handleClient();
    serviceTimeSync();

    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck >= 1000) {  // unsigned arithmetic, so millis() rollover is fine
        lastCheck = now;
        serviceSchedule();
    }
}
