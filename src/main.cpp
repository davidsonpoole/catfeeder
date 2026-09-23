#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
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

#define MEAL_NEVER_FIRED (-1L)

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

Meal meals[MAX_MEALS];
int numMeals = 0;

// Writes the schedule to flash. Returns false if it did not stick, so the
// caller can tell the client its change will not survive a reboot.
static bool saveSchedule() {
    if (!prefs.putUChar(NVS_KEY_VERSION, SCHEDULE_FORMAT_VERSION)) {
        Serial.println("WARNING: could not write schedule version to NVS");
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
        Serial.println("WARNING: could not persist meal schedule to NVS");
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
            Serial.printf("Ignoring saved schedule in unknown format %u\n", version);
        }
        return;
    }

    size_t bytes = prefs.getBytesLength(NVS_KEY_MEALS);
    if (bytes == 0) return;

    if (bytes % sizeof(StoredMeal) != 0 || bytes > sizeof(StoredMeal) * MAX_MEALS) {
        Serial.printf("Ignoring saved schedule of implausible size %u\n", (unsigned)bytes);
        return;
    }

    StoredMeal stored[MAX_MEALS];
    if (prefs.getBytes(NVS_KEY_MEALS, stored, bytes) != bytes) {
        Serial.println("WARNING: could not read saved schedule from NVS");
        return;
    }

    int count = (int)(bytes / sizeof(StoredMeal));
    for (int i = 0; i < count; i++) {
        if (stored[i].timeOfDay < 0 || stored[i].timeOfDay >= MINUTES_PER_DAY ||
            stored[i].portions < 1 || stored[i].portions > MAX_PORTIONS) {
            Serial.printf("Dropping out-of-range saved meal %d\n", i);
            continue;
        }

        meals[numMeals].portions = stored[i].portions;
        meals[numMeals].timeOfDay = stored[i].timeOfDay;
        meals[numMeals].lastFiredDay = MEAL_NEVER_FIRED;
        numMeals++;
    }

    Serial.printf("Restored %d meal(s) from flash\n", numMeals);
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

// TODO: drive the dispenser. Stubbed until the auger hardware is wired up.
static void MealEvent(const Meal& meal) {
    Serial.printf("MealEvent: serving %d portion(s) (scheduled for %02d:%02d)\n", meal.portions,
                  meal.timeOfDay / 60, meal.timeOfDay % 60);
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

static void sendJson(int code, const JsonDocument& doc) {
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

// Reads the request body as JSON. Returns false (and answers the request) when
// the body is missing or malformed.
static bool readBody(JsonDocument& doc) {
    if (!server.hasArg("plain")) {
        sendError(400, "Missing request body");
        return false;
    }

    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
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

    meals[numMeals++] = meal;
    armSchedule();  // don't serve a meal whose time passed before it existed
    bool persisted = saveSchedule();
    Serial.printf("Added meal: %d portions at %d\n", meal.portions, meal.timeOfDay);

    JsonDocument res;
    res["index"] = numMeals - 1;
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

    meals[index] = meal;
    armSchedule();
    bool persisted = saveSchedule();
    Serial.printf("Changed meal %d: %d portions at %d\n", index, meal.portions, meal.timeOfDay);

    JsonDocument res;
    res["index"] = index;
    res["persisted"] = persisted;
    mealToJson(meal, res["meal"].to<JsonObject>());
    sendJson(200, res);
}

// DELETE /api/meals/<index> -> drop a meal, closing the gap it leaves.
void handleDeleteMeal() {
    int index = mealIndexFromUri();
    if (index < 0) return;

    Meal removed = meals[index];

    for (int i = index; i < numMeals - 1; i++) {
        meals[i] = meals[i + 1];
    }
    numMeals--;

    bool persisted = saveSchedule();
    Serial.printf("Deleted meal %d\n", index);

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
void handleSetTime() {
    JsonDocument doc;
    if (!readBody(doc)) return;

    if (!doc["epoch"].is<long long>()) {
        sendError(400, "Expected integer field 'epoch' (seconds since 1970, UTC)");
        return;
    }

    long long epoch = doc["epoch"];
    if (epoch < MIN_VALID_EPOCH) {
        sendError(400, "'epoch' is implausibly far in the past");
        return;
    }

    long offset = 0;
    if (!doc["tzOffsetMinutes"].isNull()) {
        if (!doc["tzOffsetMinutes"].is<int>()) {
            sendError(400, "'tzOffsetMinutes' must be an integer");
            return;
        }
        offset = (long)doc["tzOffsetMinutes"].as<int>();
        if (offset < -MAX_TZ_OFFSET_MINUTES || offset > MAX_TZ_OFFSET_MINUTES) {
            sendError(400, "'tzOffsetMinutes' must be between -840 and 840");
            return;
        }
    }

    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    tzOffsetMinutes = offset;
    lastSyncEpoch = (time_t)epoch;
    clockSynced = true;
    armSchedule();

    time_t local = localNow();
    Serial.printf("Clock synced: local time %02d:%02d (UTC%+ld:%02ld)\n",
                  localMinuteOfDay(local) / 60, localMinuteOfDay(local) % 60, offset / 60,
                  labs(offset) % 60);

    JsonDocument res;
    clockToJson(res.to<JsonObject>());
    sendJson(200, res);
}

void handleNotFound() {
    sendError(404, "No such endpoint");
}

void setup() {
    Serial.begin(115200);

    if (prefs.begin(NVS_NAMESPACE, false)) {
        loadSchedule();
    } else {
        Serial.println("WARNING: could not open NVS; schedule will not persist");
    }

    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("Connected, IP: " + WiFi.localIP().toString());

    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/meals", HTTP_GET, handleGetSettings);
    server.on("/api/meals", HTTP_POST, handleAddMeal);
    server.on(UriBraces("/api/meals/{}"), HTTP_PUT, handleChangeMeal);
    server.on(UriBraces("/api/meals/{}"), HTTP_DELETE, handleDeleteMeal);
    server.on("/api/time", HTTP_GET, handleGetTime);
    server.on("/api/time", HTTP_POST, handleSetTime);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.printf("Listening for HTTP on port %d\n", HTTP_PORT);
    Serial.println("Clock not set: waiting for POST /api/time before feeding");
}

void loop() {

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection lost! Reconnecting...");
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        Serial.println("Connected, IP: " + WiFi.localIP().toString());
    }

    server.handleClient();

    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck >= 1000) {  // unsigned arithmetic, so millis() rollover is fine
        lastCheck = now;
        serviceSchedule();
    }
}
