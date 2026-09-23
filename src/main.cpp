#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <uri/UriBraces.h>

#include "wifi_secrets.h"

#define HTTP_PORT 80
#define MAX_MEALS 8
#define MAX_PORTIONS 10
#define MINUTES_PER_DAY 1440

typedef struct {

    int portions;
    int timeOfDay;  // minutes since midnight

} Meal;

WebServer server(HTTP_PORT);

Meal meals[MAX_MEALS];
int numMeals = 0;

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
    Serial.printf("Added meal: %d portions at %d\n", meal.portions, meal.timeOfDay);

    JsonDocument res;
    res["index"] = numMeals - 1;
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

    meals[index] = meal;
    Serial.printf("Changed meal %d: %d portions at %d\n", index, meal.portions, meal.timeOfDay);

    JsonDocument res;
    res["index"] = index;
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

    Serial.printf("Deleted meal %d\n", index);

    JsonDocument res;
    mealToJson(removed, res["meal"].to<JsonObject>());
    sendJson(200, res);
}

void handleNotFound() {
    sendError(404, "No such endpoint");
}

void setup() {
    Serial.begin(115200);
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
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.printf("Listening for HTTP on port %d\n", HTTP_PORT);
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
}
