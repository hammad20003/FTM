#include "FS.h"
#include <WiFi.h>
#include <vector>
#include "SPIFFS.h"
#include <ESPmDNS.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>      // Updated for ArduinoJson v6
#include <WebSocketsServer.h> // New include for WebSockets
#include "HX711.h"            // New include for HX711 load cell amplifier

// WiFi network credentials (handled by WiFiManager, so commented out)
//const char* ssid = "hadi";
//const char* password = "12345678";

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81); // WebSocket server on port 81

// --- Load cell pins and calibration ---
#define DT_PIN 3  // Data pin for HX711
#define SCK_PIN 2 // Clock pin for HX711
HX711 scale;
float calibration_factor = -475.31; // Calibration factor for your load cell

// --- Stepper Motor Pins ---
const int STEP_PIN = 19;
const int DIR_PIN = 18;
const int ENABLE_PIN = 5;
const int stepsPerRevolution = 400;
float distancePerStep = 9.11 / 400.0; // cm per step for the stepper motor

// Set this to 'true' if your stepper driver's ENABLE pin is active HIGH (e.g., HIGH enables, LOW disables).
// Set to 'false' if your stepper driver's ENABLE pin is active LOW (e.g., LOW enables, HIGH disables - common for A4988/DRV8825).
const bool STEPPER_ENABLE_ACTIVE_HIGH = true; 

// Base delay for stepper motor pulses (microseconds). Smaller value = faster.
// Adjust this value to control the overall speed range.
const unsigned long BASE_STEPPER_DELAY_US = 500; 

// --- Servo Motor ---
#define SERVO_PIN 7 // Pin for servo control (Updated from 13 to 7)
Servo myServo;      // Servo object (Renamed from 'servo' to 'myServo')
const int SERVO_RETRACT_ANGLE = 0;        // Angle for servo retracted position
const int SERVO_APPLY_PRESSURE_ANGLE = 90; // Angle for servo applying pressure (example)

// Test parameters for simulation (re-added for graph displacement calculation)
const float DEGREE_TO_DISPLACEMENT = 0.0216; // cm/degree (simulated, used for graph X-axis)

// Session state
bool isLoggedIn = false;
bool isAdmin = false;

// Configuration structure
struct Configuration {
  String name;
  String distance;
  String angle;
  String speed;
};

// Stored configurations
std::vector<Configuration> configurations;

// Currently loaded configuration for process
String currentConfigName = "No Configuration Loaded";
String currentDistance = "N/A";
String currentAngle = "N/A";
String currentSpeed = "N/A"; // Added to hold the speed setting

// Selected configuration for deletion
String selectedConfigNameForDelete = "No Configuration Selected";
String selectedDistanceForDelete = "N/A";
String selectedAngleForDelete = "N/A";
String selectedSpeedForDelete = "N/A";
int selectedConfigIndexForDelete = -1;

// SPIFFS file for configurations
const char* CONFIG_FILE = "/configs.json";

// Process simulation state
String processStatus = "Ready"; // Ready, Running, Paused, Stopped, Completed
unsigned long processStartTime = 0;
unsigned long processPauseTime = 0;
unsigned long processDurationSeconds = 0; // Duration based on 'angle' config
unsigned long remainingTimeSeconds = 0;
bool supplyIsOn = false;
float simulatedVoltageReading = 0.0; // No longer simulated, but kept for JSON structure
float simulatedCurrentReading = 0.0; // No longer simulated, but kept for JSON structure
unsigned long lastRealTimeUpdate = 0; // For updating remaining time
unsigned long lastStepTime = 0;       // For sending data points to WebSocket

int angle = 0; // Current angle for the simulated test, used for graph data (servo position)

// For storing readings history (for Service Mode)
String forceHistory = "";
int readingCounter = 0;

// --- Physical Reset Button ---
#define RESET_BUTTON_PIN 4
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

// mDNS status flag (re-added)
bool mDNSStarted = false;

// Convert all configurations to JSON string
String serializeAllConfigsToJson(const std::vector<Configuration>& configs) {
  // Use StaticJsonDocument for fixed size, or adjust size as needed
  StaticJsonDocument<1024> doc; 
  JsonArray jsonArray = doc.to<JsonArray>();

  for (const auto& config : configs) {
    JsonObject obj = jsonArray.add<JsonObject>();
    obj["name"] = config.name;
    obj["distance"] = config.distance;
    obj["angle"] = config.angle;
    obj["speed"] = config.speed;
  }

  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

// Populate configurations from JSON string
bool deserializeJsonToAllConfigs(const String& jsonString, std::vector<Configuration>& configs) {
  // Use StaticJsonDocument for fixed size, or or adjust size as needed
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonString);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return false;
  }

  JsonArray jsonArray = doc.as<JsonArray>();
  if (jsonArray.isNull()) return false;

  configs.clear();
  for (JsonObject obj : jsonArray) {
    Configuration config;
    config.name = obj["name"].as<String>();
    config.distance = obj["distance"].as<String>();
    config.angle = obj["angle"].as<String>();
    config.speed = obj["speed"].as<String>();
    configs.push_back(config);
  }
  return true;
}

// Save configurations to SPIFFS
void saveConfigurationsToSPIFFS() {
  String json = serializeAllConfigsToJson(configurations);
  File file = SPIFFS.open(CONFIG_FILE, "w");
  if (!file) {
    Serial.println("Failed to open config file for writing");
    return;
  }
  if (file.print(json)) {
    Serial.println("Configurations saved to SPIFFS.");
  } else {
    Serial.println("Failed to write configurations.");
  }
  file.close();
}

// Load configurations from SPIFFS
void loadConfigurationsFromSPIFFS() {
  if (!SPIFFS.exists(CONFIG_FILE)) {
    Serial.println("Config file not found. Starting with empty configs.");
    return;
  }

  File file = SPIFFS.open(CONFIG_FILE, "r");
  if (!file) {
    Serial.println("Failed to open config file for reading");
    return;
  }

  String jsonString = file.readString();
  file.close();

  if (deserializeJsonToAllConfigs(jsonString, configurations)) {
    Serial.println("Configurations loaded from SPIFFS.");
  } else {
    Serial.println("Failed to parse configurations from SPIFFS.");
  }
}

// Display a simple message box (custom modal alternative to alert/confirm)
void showMessageBox(const String& title, const String& message, const String& backLink) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>%TITLE%</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; }
    </style>
</head>
<body class="min-h-screen flex items-center justify-center p-4">
    <div class="bg-white rounded-lg shadow-xl p-8 text-center max-w-sm w-full">
        <h2 class="text-2xl font-bold text-gray-900 mb-4">%TITLE%</h2>
        <p class="text-gray-700 mb-6">%MESSAGE%</p>
        <a href="%BACK_LINK%" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out">
            Go Back
        </a>
    </div>
</body>
</html>
  )rawliteral";
  html.replace("%TITLE%", title);
  html.replace("%MESSAGE%", message);
  html.replace("%BACK_LINK%", backLink);
  server.send(200, "text/html", html);
}

// Handle login page display
void handleLogin() {
  if (isLoggedIn) {
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
    return;
  }
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FTM-01 Login</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #e2e8f0; } </style>
</head>
<body class="min-h-screen flex items-center justify-center p-4">
    <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-center md:items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
        <div class="flex flex-col space-y-6 w-full md:w-1/2">
            <h2 class="text-2xl font-bold text-gray-800">Login</h2>
            <form action="/login" method="POST" class="space-y-4">
                <div>
                    <label for="username" class="block text-sm font-medium text-gray-700 mb-1">Username *</label>
                    <input type="text" id="username" name="username" placeholder="Enter Username" class="mt-1 block w-full border-gray-300 rounded-md shadow-sm focus:ring-blue-500 focus:border-blue-500 sm:text-sm p-2" required>
                </div>
                <div>
                    <label for="password" class="block text-sm font-medium text-gray-700 mb-1">Password *</label>
                    <input type="password" id="password" name="password" placeholder="Enter Password" class="mt-1 block w-full border-gray-300 rounded-md shadow-sm focus:ring-blue-500 focus:border-blue-500 sm:text-sm p-2" required>
                </div>
                <div class="flex justify-end">
                    <button type="submit" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-2 px-6 rounded-md shadow-md transition duration-300 ease-in-out">
                        LOGIN
                    </button>
                </div>
            </form>
            <div id="errorMessage" class="text-red-600 text-sm mt-2"></div>
        </div>
        <div class="text-gray-700 w-full md:w-1/2">
            <h2 class="text-3xl font-bold text-gray-900 mb-2">FTM-01</h2>
            <h3 class="text-xl font-medium text-blue-700 mb-4">Flexural testing Machine</h3>
            <p class="text-base leading-relaxed">
            Flexural Testing Machine is designed to evaluate the bending
            strength and flexibility of catheter tubes. This system ensures
            precise measurement of mechanical performance, critical for
            maintaining quality and reliability in medical-grade applications.
            </p>
            <p class="text-sm text-gray-500 mt-4"> Copyright © Revive Medical Technologies Inc. </p>
        </div>
    </div>
    <script>
        const urlParams = new URLSearchParams(window.location.search);
        if (urlParams.has('error') && urlParams.get('error') === '1') {
            document.getElementById('errorMessage').textContent = 'Invalid username or password.';
        }
    </script>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", html);
}

// Handle login form submission
void handleLoginPost() {
  String username = server.arg("username");
  String password = server.arg("password");

  if (username == "admin" && password == "admin") {
    isLoggedIn = true;
    isAdmin = true;
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
  } else if (username == "user" && password == "user") {
    isLoggedIn = true;
    isAdmin = false;
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
  } else {
    server.sendHeader("Location", "/?error=1");
    server.send(302, "text/plain", "");
  }
}

// Handle logout
void handleLogout() {
  isLoggedIn = false;
  isAdmin = false;
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// Route to home page based on user role
void handleHomeRouter() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  String html;
  if (isAdmin) {
    html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FTM-01 Main Menu (Admin)</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; } </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <h1 class="text-xl font-semibold text-gray-800">Main Menu (Admin)</h1>
            <a href="/logout" class="bg-gray-200 hover:bg-gray-300 text-gray-700 font-semibold py-2 px-4 rounded-lg shadow-md transition duration-300 ease-in-out text-center text-sm">Logout</a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-center md:items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <div class="flex flex-col space-y-4 w-full md:w-auto">
                <a href="/page1" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out text-center"> Load Configuration </a>
                <a href="/page2" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out text-center"> Create Configuration </a>
                <a href="/service-mode" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out text-center"> Service Mode </a>
                <a href="/delete-config-page" class="bg-red-600 hover:bg-red-700 text-white font-semibold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out text-center"> Delete Configuration </a>
            </div>
            <div class="text-gray-700 w-full md:w-2/3">
                <h2 class="text-3xl font-bold text-gray-900 mb-2">FTM-01</h2>
                <h3 class="text-xl font-medium text-blue-700 mb-4">Flexural testing Machine</h3>
                <p class="text-base leading-relaxed"> Flexural Testing Machine is designed to evaluate the bending strength and flexibility of catheter tubes. This system ensures precise measurement of mechanical performance, critical for maintaining quality and reliability in medical-grade applications. </p>
                <p class="text-sm text-gray-500 mt-4"> Copyright © Revive Medical Technologies Inc. </p>
            </div>
        </div>
    </main>
</body>
</html>
    )rawliteral";
  } else {
    html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FTM-01 Main Menu (User)</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; } </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <h1 class="text-xl font-semibold text-gray-800">Main Menu (User)</h1>
            <a href="/logout" class="bg-gray-200 hover:bg-gray-300 text-gray-700 font-semibold py-2 px-4 rounded-lg shadow-md transition duration-300 ease-in-out text-center text-sm">Logout</a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-center md:items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <div class="flex flex-col space-y-4 w-full md:w-auto">
                <a href="/page1" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out text-center"> Load Configuration </a>
                <a href="/service-mode" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out text-center"> Service Mode </a>
            </div>
            <div class="text-gray-700 w-full md:w-2/3">
                <h2 class="text-3xl font-bold text-gray-900 mb-2">FTM-01</h2>
                <h3 class="text-xl font-medium text-blue-700 mb-4">Flexural testing Machine</h3>
                <p class="text-base leading-relaxed"> Flexural Testing Machine is designed to evaluate the bending strength and flexibility of catheter tubes. This system ensures precise measurement of mechanical performance, critical for maintaining quality and reliability in medical-grade applications. </p>
                <p class="text-sm text-gray-500 mt-4"> Copyright © Revive Medical Technologies Inc. </p>
            </div>
        </div>
    </main>
</body>
</html>
    )rawliteral";
  }
  server.send(200, "text/html", html);
}

// Handle Load Configuration page
void handlePage1() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  String configListHtml = "";
  if (configurations.empty()) {
    configListHtml = "<p class=\"text-gray-600\">No saved configurations found.</p>";
  } else {
    configListHtml += "<ul class=\"list-disc pl-5 space-y-2\">";
    for (size_t i = 0; i < configurations.size(); ++i) {
      configListHtml += "<li class=\"flex justify-between items-center bg-gray-50 p-2 rounded-md shadow-sm\">";
      configListHtml += "<span class=\"text-gray-800 font-medium\">" + configurations[i].name + "</span>";
      configListHtml += "<a href=\"/load-selected-config?index=" + String(i) + "\" class=\"bg-blue-500 hover:bg-blue-600 text-white text-sm font-semibold py-1 px-3 rounded-md transition duration-300 ease-in-out\">Load</a>";
      configListHtml += "</li>";
    }
    configListHtml += "</ul>";
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Load Configuration</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; } </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <div class="flex items-center space-x-4">
                <a href="/home" class="text-gray-600 hover:text-gray-900">
                    <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M10.5 19.5L3 12m0 0l7.5-7.5M3 12h18" /> </svg>
                </a>
                <h1 class="text-xl font-semibold text-gray-800">Load Configuration</h1>
            </div>
            <a href="/home" class="text-gray-600 hover:text-red-600">
                <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" /> </svg>
            </a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <div class="flex-1 w-full md:w-1/2 space-y-4">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Saved Configurations</h2>
                %CONFIG_LIST_HTML%
            </div>
            <div class="flex-1 w-full md:w-1/2 space-y-6">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Currently Loaded Configuration Details</h2>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Configuration Name</label>
                    <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_CONFIG_NAME%</p>
                </div>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Distance (cm)</label>
                    <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_DISTANCE%cm</p>
                </div>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Angle</label>
                    <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_ANGLE%</p>
                </div>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Speed</label>
                    <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_SPEED%</p>
                </div>
                <div class="flex justify-end mt-6">
                    <a href="/process" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-2 px-6 rounded-md shadow-md transition duration-300 ease-in-out"> Process </a>
                </div>
            </div>
        </div>
    </main>
</body>
</html>
  )rawliteral";
  html.replace("%CONFIG_LIST_HTML%", configListHtml);
  html.replace("%CURRENT_CONFIG_NAME%", currentConfigName);
  html.replace("%CURRENT_DISTANCE%", currentDistance + (currentDistance == "N/A" ? "" : "cm"));
  html.replace("%CURRENT_ANGLE%", currentAngle);
  html.replace("%CURRENT_SPEED%", currentSpeed);
  server.send(200, "text/html", html);
}

// Handle Create Configuration page
void handlePage2() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  if (!isAdmin) {
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
    return;
  }

  if (server.method() == HTTP_POST) {
    if (configurations.size() >= 5) {
      showMessageBox("Error", "Max 5 configurations allowed. Delete existing.", "/page2");
      return;
    }

    Configuration newConfig;
    if (server.hasArg("configName")) newConfig.name = server.arg("configName");
    if (server.hasArg("distance")) newConfig.distance = server.arg("distance");
    if (server.hasArg("angle")) newConfig.angle = server.arg("angle");
    if (server.hasArg("speed")) newConfig.speed = server.arg("speed");

    bool nameExists = false;
    for (const auto& config : configurations) {
      if (config.name == newConfig.name && !newConfig.name.isEmpty()) {
        nameExists = true;
        break;
      }
    }

    if (nameExists) {
      showMessageBox("Error", "Configuration name exists. Choose different.", "/page2");
      return;
    }

    configurations.push_back(newConfig);
    saveConfigurationsToSPIFFS();
  }

  String configListHtml = "";
  if (configurations.empty()) {
    configListHtml = "<p class=\"text-gray-600\">No configurations created yet.</p>";
  } else {
    configListHtml += "<ul class=\"list-disc pl-5 space-y-2\">";
    for (size_t i = 0; i < configurations.size(); ++i) {
      configListHtml += "<li class=\"flex justify-between items-center bg-gray-50 p-2 rounded-md shadow-sm\">";
      configListHtml += "<span class=\"text-gray-800 font-medium\">" + configurations[i].name + "</span>";
      configListHtml += "</li>";
    }
    configListHtml += "</ul>";
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Create Configuration</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; } </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <div class="flex items-center space-x-4">
                <a href="/home" class="text-gray-600 hover:text-gray-900">
                    <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M10.5 19.5L3 12m0 0l7.5-7.5M3 12h18" /> </svg>
                </a>
                <h1 class="text-xl font-semibold text-gray-800">Create Configuration</h1>
            </div>
            <a href="/home" class="text-gray-600 hover:text-red-600">
                <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" /> </svg>
            </a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <div class="flex-1 w-full md:w-1/2 space-y-6">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Create New Configuration</h2>
                <form action="/page2" method="POST" class="space-y-6">
                    <div>
                        <label for="configName" class="block text-sm font-medium text-gray-700 mb-1">Configuration Name</label>
                        <input type="text" id="configName" name="configName" placeholder="Enter Configuration Name" class="mt-1 block w-full border-gray-300 rounded-md shadow-sm focus:ring-blue-500 focus:border-blue-500 sm:text-sm p-2" required>
                    </div>
                    <div class="grid grid-cols-1 md:grid-cols-2 gap-6">
                        <div>
                            <label for="distance" class="block text-sm font-medium text-gray-700 mb-1">Distance (cm)</label>
                            <select id="distance" name="distance" class="mt-1 block w-full pl-3 pr-10 py-2 text-base border-gray-300 focus:outline-none focus:ring-blue-500 focus:border-blue-500 sm:text-sm rounded-md shadow-sm">
                                <script> for (let i = 10; i <= 90; i += 10) { document.write(`<option value="${i}">${i}cm</option>`); } </script>
                            </select>
                        </div>
                        <div>
                            <label for="angle" class="block text-sm font-medium text-gray-700 mb-1">Angle</label>
                            <select id="angle" name="angle" class="mt-1 block w-full pl-3 pr-10 py-2 text-base border-gray-300 focus:outline-none focus:ring-blue-500 focus:border-blue-500 sm:text-sm rounded-md shadow-sm">
                                <script> for (let i = 2; i <= 30; i += 2) { document.write(`<option value="${i}">${i}</option>`); } </script>
                            </select>
                        </div>
                        <div>
                            <label for="speed" class="block text-sm font-medium text-gray-700 mb-1">Speed</label>
                            <select id="speed" name="speed" class="mt-1 block w-full pl-3 pr-10 py-2 text-base border-gray-300 focus:outline-none focus:ring-blue-500 focus:border-blue-500 sm:text-sm rounded-md shadow-sm">
                                <option value="1x">1x</option> <option value="1.5x">1.5x</option> <option value="2x">2x</option>
                            </select>
                        </div>
                    </div>
                    <div class="flex justify-end mt-6">
                        <button type="submit" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-2 px-6 rounded-md shadow-md transition duration-300 ease-in-out"> Create </button>
                    </div>
                </form>
            </div>
            <div class="flex-1 w-full md:w-1/2 space-y-4">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Created Configurations</h2>
                %CONFIG_LIST_HTML%
            </div>
        </div>
    </main>
</body>
</html>
  )rawliteral";
  html.replace("%CONFIG_LIST_HTML%", configListHtml);
  server.send(200, "text/html", html);
}

// Load selected configuration into global variables
void handleLoadSelectedConfig() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < configurations.size()) {
      currentConfigName = configurations[index].name;
      currentDistance = configurations[index].distance;
      currentAngle = configurations[index].angle;
      currentSpeed = configurations[index].speed; // Load speed here
      processDurationSeconds = currentAngle.toInt(); // Angle maps to process duration
      remainingTimeSeconds = processDurationSeconds;
      processStatus = "Ready";
      supplyIsOn = false;
      simulatedVoltageReading = 0.0; // Reset simulated values
      simulatedCurrentReading = 0.0; // Reset simulated values
    }
  }
  handlePage1();
}

// Handle Delete Configuration page
void handleDeleteConfigPage() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  if (!isAdmin) {
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
    return;
  }

  String configListHtml = "";
  if (configurations.empty()) {
    configListHtml = "<p class=\"text-gray-600\">No configurations to delete.</p>";
  } else {
    configListHtml += "<ul class=\"list-none p-0 m-0 space-y-2\">";
    for (size_t i = 0; i < configurations.size(); ++i) {
      configListHtml += "<li id=\"config-" + String(i) + "\" class=\"cursor-pointer bg-gray-50 hover:bg-gray-100 p-3 rounded-md shadow-sm transition duration-150 ease-in-out\" onclick=\"selectConfig(" + String(i) + ")\">";
      configListHtml += "<span class=\"text-gray-800 font-medium\">" + configurations[i].name + "</span>";
      configListHtml += "</li>";
    }
    configListHtml += "</ul>";
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Delete Configuration</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; }
        .selected-config { background-color: #bfdbfe !important; border: 1px solid #60a5fa; }
    </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <div class="flex items-center space-x-4">
                <a href="/home" class="text-gray-600 hover:text-gray-900">
                    <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M10.5 19.5L3 12m0 0l7.5-7.5M3 12h18" /> </svg>
                </a>
                <h1 class="text-xl font-semibold text-gray-800">Delete Configuration</h1>
            </div>
            <a href="/home" class="text-gray-600 hover:text-red-600">
                <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" /> </svg>
            </a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <div class="flex-1 w-full md:w-1/2 space-y-4">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Saved Configurations</h2>
                %CONFIG_LIST_HTML%
                <div class="flex justify-end mt-6">
                    <button id="deleteButton" onclick="confirmDelete()" class="bg-red-600 hover:bg-red-700 text-white font-semibold py-2 px-6 rounded-md shadow-md transition duration-300 ease-in-out opacity-50 cursor-not-allowed" disabled> Delete Selected Configuration </button>
                </div>
            </div>
            <div class="flex-1 w-full md:w-1/2 space-y-6">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Selected Configuration Details</h2>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Configuration Name</label>
                    <p id="displayConfigName" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%SELECTED_CONFIG_NAME_FOR_DELETE%</p>
                </div>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Distance (cm)</label>
                    <p id="displayDistance" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%SELECTED_DISTANCE_FOR_DELETE%</p>
                </div>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Angle</label>
                    <p id="displayAngle" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%SELECTED_ANGLE_FOR_DELETE%</p>
                </div>
                <div>
                    <label class="block text-sm font-medium text-gray-700 mb-1">Speed</label>
                    <p id="displaySpeed" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%SELECTED_SPEED_FOR_DELETE%</p>
                </div>
            </div>
        </div>
    </main>
    <script>
        let selectedIndex = -1;
        const configurationsData = %CONFIGURATIONS_JSON%; // This will be replaced by C++

        function selectConfig(index) {
            // Remove 'selected-config' from previously selected item
            if (selectedIndex !== -1 && document.getElementById('config-' + selectedIndex)) {
                document.getElementById('config-' + selectedIndex).classList.remove('selected-config');
            }

            // Add 'selected-config' to the new selected item
            const selectedElement = document.getElementById('config-' + index);
            if (selectedElement) {
                selectedElement.classList.add('selected-config');
                selectedIndex = index;
                document.getElementById('deleteButton').disabled = false;
                document.getElementById('deleteButton').classList.remove('opacity-50', 'cursor-not-allowed');

                // Update displayed details
                const config = configurationsData[index];
                document.getElementById('displayConfigName').textContent = config.name;
                document.getElementById('displayDistance').textContent = config.distance + 'cm';
                document.getElementById('displayAngle').textContent = config.angle;
                document.getElementById('displaySpeed').textContent = config.speed;
            }
        }

        function confirmDelete() {
            if (selectedIndex !== -1) {
                // Instead of confirm(), use a custom modal or direct action
                // For this example, we'll directly navigate to the delete URL
                // In a real application, you'd show a custom confirmation dialog
                window.location.href = "/confirm-delete-config?index=" + selectedIndex;
            }
        }

        // Initialize display with "No Configuration Selected" if no config is selected
        document.addEventListener('DOMContentLoaded', () => {
             if (selectedIndex === -1) {
                document.getElementById('displayConfigName').textContent = "No Configuration Selected";
                document.getElementById('displayDistance').textContent = "N/A";
                document.getElementById('displayAngle').textContent = "N/A";
                document.getElementById('displaySpeed').textContent = "N/A";
            }
        });
    </script>
</body>
</html>
  )rawliteral";
  // Replace placeholders for delete page
  html.replace("%CONFIG_LIST_HTML%", configListHtml);
  html.replace("%SELECTED_CONFIG_NAME_FOR_DELETE%", selectedConfigNameForDelete);
  html.replace("%SELECTED_DISTANCE_FOR_DELETE%", selectedDistanceForDelete + (selectedDistanceForDelete == "N/A" ? "" : "cm"));
  html.replace("%SELECTED_ANGLE_FOR_DELETE%", selectedAngleForDelete);
  html.replace("%SELECTED_SPEED_FOR_DELETE%", selectedSpeedForDelete);
  html.replace("%CONFIGURATIONS_JSON%", serializeAllConfigsToJson(configurations)); // Pass all configs as JSON for JS to use
  server.send(200, "text/html", html);
}

// Handle deletion of selected configuration
void handleDeleteSelectedConfig() {
  if (!isLoggedIn || !isAdmin) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < configurations.size()) {
      selectedConfigNameForDelete = configurations[index].name;
      selectedDistanceForDelete = configurations[index].distance;
      selectedAngleForDelete = configurations[index].angle;
      selectedSpeedForDelete = configurations[index].speed;
      selectedConfigIndexForDelete = index; // Store index for actual deletion
      server.sendHeader("Location", "/confirm-delete-config"); // Redirect to confirmation
      server.send(302, "text/plain", "");
      return;
    }
  }
  showMessageBox("Error", "Invalid configuration selected.", "/delete-config-page");
}

// Handle confirmation of deletion
void handleConfirmDeleteConfig() {
  if (!isLoggedIn || !isAdmin) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  if (server.method() == HTTP_POST && server.hasArg("confirm") && server.arg("confirm") == "yes") {
    if (selectedConfigIndexForDelete != -1 && selectedConfigIndexForDelete < configurations.size()) {
      // Remove the configuration
      configurations.erase(configurations.begin() + selectedConfigIndexForDelete);
      saveConfigurationsToSPIFFS(); // Save changes
      // Reset selected config details
      selectedConfigNameForDelete = "No Configuration Selected";
      selectedDistanceForDelete = "N/A";
      selectedAngleForDelete = "N/A";
      selectedSpeedForDelete = "N/A";
      selectedConfigIndexForDelete = -1;
      showMessageBox("Success", "Configuration deleted successfully.", "/delete-config-page");
      return;
    } else {
      showMessageBox("Error", "No configuration selected for deletion.", "/delete-config-page");
      return;
    }
  }

  // Display confirmation page
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Confirm Deletion</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; } </style>
</head>
<body class="min-h-screen flex items-center justify-center p-4">
    <div class="bg-white rounded-lg shadow-xl p-8 text-center max-w-sm w-full">
        <h2 class="text-2xl font-bold text-gray-900 mb-4">Confirm Deletion</h2>
        <p class="text-gray-700 mb-6">Are you sure you want to delete configuration: <br><span class="font-semibold">%CONFIG_NAME%</span>?</p>
        <form action="/confirm-delete-config" method="POST" class="space-y-4">
            <input type="hidden" name="confirm" value="yes">
            <div class="flex justify-center space-x-4">
                <button type="submit" class="bg-red-600 hover:bg-red-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out">
                    Yes, Delete
                </button>
                <a href="/delete-config-page" class="bg-gray-300 hover:bg-gray-400 text-gray-800 font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out">
                    Cancel
                </a>
            </div>
        </form>
    </div>
</body>
</html>
  )rawliteral";
  html.replace("%CONFIG_NAME%", selectedConfigNameForDelete);
  server.send(200, "text/html", html);
}

// This function handles the /process route and displays the process page
void handleProcessPage() {
  // Ensure the user is logged in before accessing this page
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  // HTML for the Process Mode page, including the graph visualization area
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FTM-01 Process Mode</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; }
        /* Custom styling for the section boxes */
        .section-box {
            min-height: 250px; /* Ensure boxes have some height */
            border-radius: 0.5rem; /* Rounded corners */
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06); /* Subtle shadow */
        }
    </style>
    <!-- Chart.js library for graph visualization -->
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <div class="flex items-center space-x-4">
                <a href="/page1" class="text-gray-600 hover:text-gray-900">
                    <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M10.5 19.5L3 12m0 0l7.5-7.5M3 12h18" /> </svg>
                </a>
                <h1 class="text-xl font-semibold text-gray-800">Process Mode</h1>
            </div>
            <a href="/home" class="text-gray-600 hover:text-red-600">
                <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" /> </svg>
            </a>
        </nav>
    </header>

    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col lg:flex-row items-start space-y-8 lg:space-y-0 lg:space-x-8 max-w-6xl w-full">

            <!-- User-Defined Values Section -->
            <div class="section-box flex-1 p-6 bg-gray-50 border border-gray-200">
                <h2 class="text-lg font-semibold text-gray-800 mb-4">User-Defined Values</h2>
                <div class="space-y-3">
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Name</label>
                        <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_CONFIG_NAME%</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Distance</label>
                        <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_DISTANCE%cm</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Angle</label>
                        <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_ANGLE%</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Speed</label>
                        <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_SPEED%</p>
                    </div>
                </div>
            </div>

            <!-- Graph/Visual Area Section (Updated) -->
            <div class="section-box flex-1 flex items-center justify-center bg-gray-50 border border-gray-200 p-4">
                <canvas id="flexuralChart" class="w-full h-full"></canvas>
            </div>

            <!-- Real-Time Values Section -->
            <div class="section-box flex-1 p-6 bg-gray-50 border border-gray-200">
                <h2 class="text-lg font-semibold text-gray-800 mb-4">Real-Time Values</h2>
                <div class="space-y-3">
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Status</label>
                        <p id="processStatus" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%PROCESS_STATUS%</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Remaining Time</label>
                        <p id="remainingTime" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800 text-2xl font-bold text-blue-600 text-center">
                            %REMAINING_TIME_MM_SS%
                        </p>
                        <p class="text-xs text-gray-500 text-center">MM : SS</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Simulated Voltage</label>
                        <p id="simulatedVoltage" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%SIMULATED_VOLTAGE% V</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700">Simulated Current</label>
                        <p id="simulatedCurrent" class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%SIMULATED_CURRENT% A</p>
                    </div>
                </div>
            </div>
        </div>
    </main>

    <!-- Control Buttons -->
    <div class="flex justify-center p-4 space-x-4 bg-white shadow-sm mt-4 rounded-lg max-w-lg mx-auto mb-4">
        <button id="startButton" class="bg-green-600 hover:bg-green-700 text-white font-semibold py-3 px-8 rounded-md shadow-md transition duration-300 ease-in-out">START</button>
        <button id="pauseButton" class="bg-orange-500 hover:bg-orange-600 text-white font-semibold py-3 px-8 rounded-md shadow-md transition duration-300 ease-in-out">PAUSE</button>
        <button id="stopButton" class="bg-red-600 hover:bg-red-700 text-white font-semibold py-3 px-8 rounded-md shadow-md transition duration-300 ease-in-out">STOP</button>
    </div>

    <script>
        // JavaScript for the Chart and WebSocket communication
        const ctx = document.getElementById('flexuralChart').getContext('2d');
        const chartData = {
            labels: [], // Displacement values will go here
            datasets: [{
                label: 'Force (g)',
                data: [], // Force values will go here
                borderColor: 'rgb(255, 165, 0)', // Orange color
                tension: 0.1,
                fill: false,
                pointRadius: 2 // Smaller points for cleaner look
            }]
        };

        const chartConfig = {
            type: 'line',
            data: chartData,
            options: {
                animation: false, // Disable animation for real-time updates
                responsive: true,
                maintainAspectRatio: false, // Allow canvas to resize freely
                scales: {
                    x: {
                        title: {
                            display: true,
                            text: 'Displacement (cm)',
                            color: '#4A5568' // Gray-700
                        },
                        ticks: {
                            color: '#4A5568'
                        },
                        grid: {
                            color: '#E2E8F0' // Gray-200
                        }
                    },
                    y: {
                        title: {
                            display: true,
                            text: 'Force (g)',
                            color: '#4A5568'
                        },
                        ticks: {
                            color: '#4A5568'
                        },
                        grid: {
                            color: '#E2E8F0'
                        }
                    }
                },
                plugins: {
                    legend: {
                        labels: {
                            color: '#4A5568'
                        }
                    }
                }
            }
        };

        const flexuralChart = new Chart(ctx, chartConfig);

        // WebSocket connection for real-time data
        let ws;
        function connectWebSocket() {
            ws = new WebSocket("ws://" + location.hostname + ":81/");

            ws.onopen = () => {
                console.log('WebSocket connected');
            };

            ws.onmessage = (event) => {
                const [disp, force] = event.data.split(",");
                const displacement = parseFloat(disp);
                const forceValue = parseFloat(force);

                // Add new data point
                chartData.labels.push(displacement.toFixed(2)); // Display displacement on X-axis
                chartData.datasets[0].data.push(forceValue);

                // Keep only the last N data points to prevent performance issues with too much data
                const maxDataPoints = 50; // Adjust as needed
                if (chartData.labels.length > maxDataPoints) {
                    chartData.labels.shift();
                    chartData.datasets[0].data.shift();
                }

                flexuralChart.update(); // Update the chart
            };

            ws.onclose = () => {
                console.log('WebSocket disconnected. Attempting to reconnect...');
                setTimeout(connectWebSocket, 3000); // Attempt to reconnect after 3 seconds
            };

            ws.onerror = (error) => {
                console.error('WebSocket error:', error);
                ws.close(); // Close to trigger reconnect
            };
        }

        connectWebSocket(); // Initiate WebSocket connection on page load

        // --- Real-time value updates (from original code's intention) ---
        function updateRealTimeValues() {
            fetch('/get-process-status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('processStatus').textContent = data.processStatus;
                    document.getElementById('remainingTime').textContent = data.remainingTime;
                    document.getElementById('simulatedVoltage').textContent = data.simulatedVoltageReading.toFixed(2) + ' V';
                    document.getElementById('simulatedCurrent').textContent = data.simulatedCurrentReading.toFixed(2) + ' A';
                })
                .catch(error => console.error('Error fetching process status:', error));
        }

        // Update real-time values every second (adjust interval as needed)
        setInterval(updateRealTimeValues, 1000);

        // --- Button Handlers (from original code's intention) ---
        document.getElementById('startButton').addEventListener('click', () => {
            fetch('/start-process')
                .then(response => response.text())
                .then(message => {
                    console.log(message);
                    // Clear chart data when starting a new process
                    chartData.labels = [];
                    chartData.datasets[0].data = [];
                    flexuralChart.update();
                })
                .catch(error => console.error('Error starting process:', error));
        });

        document.getElementById('pauseButton').addEventListener('click', () => {
            fetch('/pause-process')
                .then(response => response.text())
                .then(message => console.log(message))
                .catch(error => console.error('Error pausing process:', error));
        });

        document.getElementById('stopButton').addEventListener('click', () => {
            fetch('/stop-process')
                .then(response => response.text())
                .then(message => console.log(message))
                .catch(error => console.error('Error stopping process:', error));
        });

    </script>
</body>
</html>
  )rawliteral";

  // Replace placeholders with current configuration and process data
  html.replace("%CURRENT_CONFIG_NAME%", currentConfigName);
  html.replace("%CURRENT_DISTANCE%", currentDistance);
  html.replace("%CURRENT_ANGLE%", currentAngle);
  html.replace("%CURRENT_SPEED%", currentSpeed);

  // Format remaining time
  int minutes = remainingTimeSeconds / 60;
  int seconds = remainingTimeSeconds % 60;
  char timeBuffer[6]; // MM:SS\0
  sprintf(timeBuffer, "%02d:%02d", minutes, seconds);
  html.replace("%REMAINING_TIME_MM_SS%", String(timeBuffer));

  // Simulated voltage/current are now 0.0 as they are not real readings
  html.replace("%SIMULATED_VOLTAGE%", String(simulatedVoltageReading, 2));
  html.replace("%SIMULATED_CURRENT%", String(simulatedCurrentReading, 2));

  server.send(200, "text/html", html);
}

// --- Functions for Stepper, Servo, and Load Cell Control ---
void moveStepper(float cmToMove) {
  // Determine direction: LOW for positive movement (forward), HIGH for negative movement (backward)
  // You might need to invert LOW/HIGH here if your motor spins the wrong way for a given direction.
  int direction = (cmToMove >= 0) ? LOW : HIGH;
  digitalWrite(DIR_PIN, direction);
  
  long steps = abs(cmToMove) / distancePerStep;

  Serial.print("Moving stepper ");
  Serial.print(cmToMove);
  Serial.print(" cm (");
  Serial.print(steps);
  Serial.println(" steps)");

  // Calculate step delay based on configured speed
  unsigned long stepperPulseDelay = BASE_STEPPER_DELAY_US; // Default to 1x speed
  if (currentSpeed == "1.5x") {
    stepperPulseDelay = BASE_STEPPER_DELAY_US * 0.75; // Adjust factor for 1.5x speed (smaller delay = faster)
  } else if (currentSpeed == "2x") {
    stepperPulseDelay = BASE_STEPPER_DELAY_US / 2; // Adjust factor for 2x speed (smaller delay = faster)
  }
  // If speed is "1x" or unrecognized, it defaults to BASE_STEPPER_DELAY_US


  // ENABLE_PIN: Control based on STEPPER_ENABLE_ACTIVE_HIGH flag
  digitalWrite(ENABLE_PIN, STEPPER_ENABLE_ACTIVE_HIGH ? HIGH : LOW); // Enable stepper driver
  delay(10); // Small delay for enable to take effect

  for (long i = 0; i < steps; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(stepperPulseDelay); 
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(stepperPulseDelay); 
  }

  // Disable stepper driver after movement
  digitalWrite(ENABLE_PIN, STEPPER_ENABLE_ACTIVE_HIGH ? LOW : HIGH); 
  Serial.println("Stepper move complete.");
}

void moveTubeOnly(float distance) {
  moveStepper(distance);
}

// Function to perform a small, slow stepper move for testing
void testStepperMove() {
  Serial.println("Performing a test stepper move (1cm forward, slow speed)...");
  int direction = LOW; // Forward direction
  digitalWrite(DIR_PIN, direction);
  
  long steps = 1.0 / distancePerStep; // Move 1 cm

  digitalWrite(ENABLE_PIN, STEPPER_ENABLE_ACTIVE_HIGH ? HIGH : LOW); // Enable stepper driver
  delay(10); // Small delay for enable to take effect

  for (long i = 0; i < steps; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(1500); // Slower speed for testing
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(1500); 
  }

  digitalWrite(ENABLE_PIN, STEPPER_ENABLE_ACTIVE_HIGH ? LOW : HIGH); // Disable stepper driver
  Serial.println("Test stepper move complete.");
}


void sendCurrentForce() {
  if (scale.is_ready()) {
    float grams = scale.get_units(10); // Get average of 10 readings
    float newtons = grams * 0.00980665; // Convert grams to Newtons
    readingCounter++;
    forceHistory += String(readingCounter) + ": " + String(newtons, 3) + " N\n";
    Serial.println("Measured Force: " + String(newtons, 3) + " N");
  } else {
    Serial.println("HX711 not ready for force reading.");
    readingCounter++;
    forceHistory += String(readingCounter) + ": HX711 not ready\n";
  }
}

void moveServo(int angle) {
  Serial.print("Moving servo to ");
  Serial.print(angle);
  Serial.println(" degrees.");
  myServo.write(angle);
  delay(1000); // Give servo time to reach position
}

// Handle Service Mode page (for direct control of hardware)
void handleServiceMode() {
  if (!isLoggedIn || !isAdmin) { // Only admin can access service mode
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FTM-01 Service Mode</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style> body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; } </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <div class="flex items-center space-x-4">
                <a href="/home" class="text-gray-600 hover:text-gray-900">
                    <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M10.5 19.5L3 12m0 0l7.5-7.5M3 12h18" /> </svg>
                </a>
                <h1 class="text-xl font-semibold text-gray-800">Service Mode</h1>
            </div>
            <a href="/home" class="text-gray-600 hover:text-red-600">
                <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" /> </svg>
            </a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <!-- Stepper Control -->
            <div class="flex-1 w-full space-y-4">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Stepper Motor Control</h2>
                <div class="space-y-2">
                    <button onclick="sendStepperCommand('/move10cm_only')" class="w-full bg-blue-500 hover:bg-blue-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Move 10cm</button>
                    <button onclick="sendStepperCommand('/move30cm_only')" class="w-full bg-blue-500 hover:bg-blue-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Move 30cm</button>
                    <button onclick="sendStepperCommand('/move60cm_only')" class="w-full bg-blue-500 hover:bg-blue-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Move 60cm</button>
                    <div class="flex space-x-2">
                        <input type="number" id="customDistance" placeholder="Custom cm" class="flex-1 border-gray-300 rounded-md shadow-sm p-2">
                        <button onclick="sendCustomStepperCommand()" class="bg-blue-500 hover:bg-blue-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Move Custom</button>
                    </div>
                    <button onclick="sendCommand('/test-stepper-move')" class="w-full bg-gray-600 hover:bg-gray-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Test Stepper (1cm Slow)</button>
                </div>
            </div>

            <!-- Servo Control -->
            <div class="flex-1 w-full space-y-4">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Servo Motor Control</h2>
                <div class="space-y-2">
                    <button onclick="sendServoCommand(0)" class="w-full bg-green-500 hover:bg-green-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Retract (0°)</button>
                    <button onclick="sendServoCommand(90)" class="w-full bg-green-500 hover:bg-green-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Apply Pressure (90°)</button>
                    <div class="flex space-x-2">
                        <input type="number" id="customServoAngle" placeholder="Custom Angle (0-180)" class="flex-1 border-gray-300 rounded-md shadow-sm p-2" min="0" max="180">
                        <button onclick="sendCustomServoCommand()" class="bg-green-500 hover:bg-green-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Set Custom Angle</button>
                    </div>
                </div>
            </div>

            <!-- Load Cell Readings -->
            <div class="flex-1 w-full space-y-4">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Load Cell Readings</h2>
                <div class="space-y-2">
                    <button onclick="readForce()" class="w-full bg-purple-500 hover:bg-purple-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Read Current Force</button>
                    <p class="text-lg font-bold text-gray-900">Current Force: <span id="currentForce">N/A</span> N</p>
                    <button onclick="getForceHistory()" class="w-full bg-purple-500 hover:bg-purple-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Get Force History</button>
                    <pre id="forceHistory" class="bg-gray-100 p-3 rounded-md text-sm overflow-auto h-48">No history yet.</pre>
                    <button onclick="resetForceData()" class="w-full bg-red-500 hover:bg-red-600 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300">Reset Force Data</button>
                </div>
            </div>
        </div>
    </main>
    <script>
        async function sendCommand(url) {
            try {
                const response = await fetch(url);
                const message = await response.text();
                console.log(message);
                // Optionally, update UI with success/error message
            } catch (error) {
                console.error('Error sending command:', error);
            }
        }

        function sendStepperCommand(path) {
            sendCommand(path);
        }

        function sendCustomStepperCommand() {
            const distance = document.getElementById('customDistance').value;
            if (distance) {
                sendCommand('/move_custom_cm_only?distance=' + distance);
            } else {
                // Using a custom message box instead of alert()
                alert('Please enter a distance.'); // Keeping alert for simplicity in service mode, but ideally custom modal
            }
        }

        function sendServoCommand(angle) {
            sendCommand('/move_servo?angle=' + angle);
        }

        function sendCustomServoCommand() {
            const angle = document.getElementById('customServoAngle').value;
            if (angle !== '' && angle >= 0 && angle <= 180) {
                sendCommand('/move_servo?angle=' + angle);
            } else {
                // Using a custom message box instead of alert()
                alert('Please enter a valid angle (0-180).'); // Keeping alert for simplicity in service mode, but ideally custom modal
            }
        }

        async function readForce() {
            try {
                const response = await fetch('/read_force');
                const message = await response.text();
                // The /read_force endpoint just triggers a print to serial and updates server-side history.
                // We rely on getForceHistory to retrieve and display the actual force.
                console.log(message);
            } catch (error) {
                console.error('Error reading force:', error);
            }
        }

        async function getForceHistory() {
            try {
                const response = await fetch('/get_history');
                const history = await response.text();
                document.getElementById('forceHistory').textContent = history || 'No history yet.';
                // If the last line of history is the current force, update currentForce span
                const lines = history.trim().split('\n');
                if (lines.length > 0 && lines[lines.length - 1].includes('N')) {
                    const lastLine = lines[lines.length - 1];
                    const match = lastLine.match(/([\d\.]+)\sN/);
                    if (match) {
                        document.getElementById('currentForce').textContent = match[1];
                    } else {
                        document.getElementById('currentForce').textContent = 'N/A';
                    }
                } else {
                    document.getElementById('currentForce').textContent = 'N/A';
                }

            } catch (error) {
                console.error('Error fetching force history:', error);
                document.getElementById('forceHistory').textContent = 'Failed to load history.';
            }
        }

        async function resetForceData() {
            try {
                const response = await fetch('/reset_force_data');
                const message = await response.text();
                console.log(message);
                document.getElementById('forceHistory').textContent = 'No history yet.';
                document.getElementById('currentForce').textContent = 'N/A';
            } catch (error) {
                console.error('Error resetting force data:', error);
            }
        }

        // Initial load of force history
        document.addEventListener('DOMContentLoaded', getForceHistory);
        // Refresh force history periodically
        setInterval(getForceHistory, 2000); // Refresh every 2 seconds
    </script>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", html);
}


// WebSocket event handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\n", num, payload);
      // If the client sends "RESET_CHART", clear server-side angle for a fresh start
      if (String((char*)payload) == "RESET_CHART") {
          angle = 0; // Reset angle for a new test run on the server side
          Serial.println("Chart reset requested by client.");
      }
      break;
    default:
      break;
  }
}


void setup() {
  Serial.begin(115200);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // --- Load Cell Setup ---
  scale.begin(DT_PIN, SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare(); // Calibrate/zero the scale
  Serial.println("Load cell ready.");

  // --- Stepper Motor Setup ---
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  // Set initial state of ENABLE pin based on configuration
  digitalWrite(ENABLE_PIN, STEPPER_ENABLE_ACTIVE_HIGH ? LOW : HIGH); // Disable stepper by default
  Serial.println("Stepper motor pins initialized.");

  // --- Servo Motor Setup ---
  myServo.attach(SERVO_PIN);
  myServo.write(SERVO_RETRACT_ANGLE); // Set initial position
  Serial.println("Servo ready.");


  // === WiFi Setup with WiFiManager ===
  WiFiManager wm;
  wm.setConfigPortalTimeout(40); // Optional: auto-close config portal
  bool connectedToRouter = wm.autoConnect("ESP32-Setup", "12345678");

  if (connectedToRouter) {
    Serial.println(" Connected to Wi-Fi!");
    Serial.print("ESP32 IP Address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("esp32-FTM")) {
      Serial.println(" mDNS started: http://esp32-FTM.local");
      mDNSStarted = true;
    } else {
      Serial.println(" Failed to start mDNS");
    }
  } else {
    Serial.println(" Failed to connect. Starting fallback AP...");
    WiFi.softAP("ESP32-DirectConnect", "12345678");
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP());
  }

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  loadConfigurationsFromSPIFFS(); // Load configs on startup

  // Server routes
  server.on("/", handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", handleLogout);
  server.on("/home", handleHomeRouter);
  server.on("/page1", handlePage1);
  server.on("/page2", handlePage2);
  server.on("/load-selected-config", handleLoadSelectedConfig);
  server.on("/delete-config-page", handleDeleteConfigPage);
  server.on("/delete-selected-config", handleDeleteSelectedConfig);
  server.on("/confirm-delete-config", handleConfirmDeleteConfig);

  // New routes for process control
  server.on("/process", handleProcessPage);
  server.on("/start-process", HTTP_GET, []() {
    if (isLoggedIn && currentConfigName != "No Configuration Loaded") {
      // 1. Move stepper motor to the specified distance
      float distanceToMove = currentDistance.toFloat();
      if (distanceToMove > 0) { // Only move if distance is positive
        Serial.print("Moving stepper to configured distance: ");
        Serial.print(distanceToMove);
        Serial.println(" cm");
        moveStepper(distanceToMove); // Blocking call, waits for stepper to finish
        Serial.println("Stepper movement complete. Starting process.");
      } else {
        Serial.println("No valid distance configured for stepper, skipping stepper move.");
      }

      // 2. Then, start the rest of the process
      processStatus = "Running";
      processStartTime = millis();
      supplyIsOn = true; // Turn on supply (conceptual)
      angle = 0; // Reset angle for a new test run on the server side
      Serial.println("Process Started");
      server.send(200, "text/plain", "Process Started (Stepper moved first)");
    } else {
      server.send(400, "text/plain", "No configuration loaded or not logged in.");
    }
  });

  server.on("/pause-process", HTTP_GET, []() {
    if (isLoggedIn && processStatus == "Running") {
      processStatus = "Paused";
      processPauseTime = millis();
      supplyIsOn = false; // Turn off supply (conceptual)
      Serial.println("Process Paused");
      server.send(200, "text/plain", "Process Paused");
    } else {
      server.send(400, "text/plain", "Process not running or not logged in.");
    }
  });

  server.on("/stop-process", HTTP_GET, []() {
    if (isLoggedIn && (processStatus == "Running" || processStatus == "Paused")) {
      processStatus = "Stopped";
      remainingTimeSeconds = 0;
      simulatedVoltageReading = 0.0; // Reset simulated values
      simulatedCurrentReading = 0.0; // Reset simulated values
      supplyIsOn = false; // Turn off supply (conceptual)
      angle = 0; // Reset angle on stop
      myServo.write(SERVO_RETRACT_ANGLE); // Return servo to initial position
      Serial.println("Process Stopped");
      server.send(200, "text/plain", "Process Stopped");
    } else {
      server.send(400, "text/plain", "Process not active or not logged in.");
    }
  });

  // Endpoint to send real-time process data (for the "Real-Time Values" section)
  server.on("/get-process-status", HTTP_GET, []() {
    String json = "{";
    json += "\"processStatus\":\"" + processStatus + "\",";
    json += "\"remainingTime\":\"";
    int minutes = remainingTimeSeconds / 60;
    int seconds = remainingTimeSeconds % 60;
    char timeBuffer[6];
    sprintf(timeBuffer, "%02d:%02d", minutes, seconds);
    json += String(timeBuffer) + "\",";
    json += "\"simulatedVoltageReading\":" + String(simulatedVoltageReading, 2) + ","; // Will be 0.0
    json += "\"simulatedCurrentReading\":" + String(simulatedCurrentReading, 2); // Will be 0.0
    json += "}";
    server.send(200, "application/json", json);
  });

  // --- Service Mode Endpoints (New) ---
  server.on("/service-mode", handleServiceMode); // Route for the new service mode page
  server.on("/get_history", []() {
    server.send(200, "text/plain", forceHistory);
  });
  server.on("/move10cm_only", []() {
    moveTubeOnly(10.0);
    server.send(200, "text/plain", "OK");
  });
  server.on("/move30cm_only", []() {
    moveTubeOnly(30.0);
    server.send(200, "text/plain", "OK");
  });
  server.on("/move60cm_only", []() {
    moveTubeOnly(60.0);
    server.send(200, "text/plain", "OK");
  });
  server.on("/move_custom_cm_only", HTTP_GET, []() {
    if (server.hasArg("distance")) {
      float distance = server.arg("distance").toFloat();
      moveTubeOnly(distance);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing distance parameter");
    }
  });
  server.on("/test-stepper-move", HTTP_GET, []() { // New endpoint for testing stepper
    testStepperMove();
    server.send(200, "text/plain", "Test stepper move initiated.");
  });
  server.on("/read_force", []() {
    sendCurrentForce();
    server.send(200, "text/plain", "OK");
  });
  server.on("/move_servo", HTTP_GET, []() {
    if (server.hasArg("angle")) {
      int angle = server.arg("angle").toInt();
      if (angle >= 0 && angle <= 180) {
        moveServo(angle);
        server.send(200, "text/plain", "OK");
      } else {
        server.send(400, "text/plain", "Invalid angle (0-180 allowed)");
      }
    } else {
      server.send(400, "text/plain", "Missing angle parameter");
    }
  });
  server.on("/reset_force_data", []() {
    forceHistory = "";
    readingCounter = 0;
    server.send(200, "text/plain", "OK");
  });
  server.on("/reset", []() { // This endpoint for full WiFi reset and reboot
    server.send(200, "text/plain", "Resetting Wi-Fi and rebooting...");
    delay(1000);
    WiFiManager wm;
    wm.resetSettings();
    delay(500);
    ESP.restart();
  });


  server.begin();
  Serial.println("Web server started.");

  // Initialize WebSocket server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent); // Register the event handler
}

void loop() {
  // Wi-Fi reset button logic (hold for 3 seconds)
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
      Serial.println("Reset button pressed. Hold for 3 seconds...");
    } else if (millis() - buttonPressTime > 3000) {
      Serial.println("⏱ Long press detected — Resetting Wi-Fi settings...");
      WiFiManager wm;
      wm.resetSettings();
      delay(500);
      ESP.restart();
    }
  } else {
    buttonPressed = false;
  }
  
  server.handleClient();
  webSocket.loop(); // Process WebSocket events

  // Process simulation logic (now with real hardware interaction)
  if (processStatus == "Running") {
    // Update real-time values (remaining time)
    if (millis() - lastRealTimeUpdate >= 100) { // Update every 100ms
      // Simulated voltage/current are no longer relevant, set to 0.0
      simulatedVoltageReading = 0.0;
      simulatedCurrentReading = 0.0;

      // Update remaining time
      unsigned long elapsed = (millis() - processStartTime) / 1000;
      if (processDurationSeconds > elapsed) {
        remainingTimeSeconds = processDurationSeconds - elapsed;
      } else {
        remainingTimeSeconds = 0;
        processStatus = "Completed"; // Mark as completed
        supplyIsOn = false; // Conceptual supply off
        simulatedVoltageReading = 0.0; // Ensure display is zeroed
        simulatedCurrentReading = 0.0; // Ensure display is zeroed
        myServo.write(SERVO_RETRACT_ANGLE); // Return servo to initial position
        Serial.println("Process Completed!");
      }
      lastRealTimeUpdate = millis();
    }

    // Send real data to WebSocket for graph and control servo
    if (millis() - lastStepTime >= 300) { // Send data point every 300ms
      if (angle <= currentAngle.toInt()) { // Continue until servo reaches target angle from config
        myServo.write(angle); // Move servo to the current 'angle'
        
        // Read actual force from load cell
        float grams = 0.0;
        if (scale.is_ready()) {
          grams = scale.get_units(10); // Get average of 10 readings
        } else {
          Serial.println("HX711 not ready for force reading. Using 0.0.");
          // Optionally, handle error or use a default value
        }

        // Displacement for the graph is still based on the 'angle'
        // This assumes 'angle' directly correlates to a physical displacement in your setup
        float displacement = angle * DEGREE_TO_DISPLACEMENT;
        String message = String(displacement, 4) + "," + String(grams, 2); // Send displacement and actual force (grams)
        webSocket.broadcastTXT(message); // Send data to all connected WebSocket clients
        
        angle++; // Increment angle for next step
        lastStepTime = millis();
      } else {
        // If the angle limit is reached, and process is still running (time-based)
        // ensure the servo stays at max angle until process is completed by time.
        myServo.write(currentAngle.toInt()); // Keep servo at max angle
      }
    }
  } else if (processStatus == "Paused") {
    // Do nothing, or handle specific pause logic (e.g., maintain servo position)
  } else if (processStatus == "Stopped" || processStatus == "Completed" || processStatus == "Ready") {
    // Ensure servo is at 0 when not running
    if (myServo.read() != SERVO_RETRACT_ANGLE) {
      myServo.write(SERVO_RETRACT_ANGLE);
    }
  }
}
