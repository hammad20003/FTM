#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <vector>
#include "FS.h"
#include "SPIFFS.h"

// WiFi network credentials
const char* ssid = "hadi";
const char* password = "12345678";

WebServer server(80);

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
String currentSpeed = "N/A";

// Selected configuration for deletion
String selectedConfigNameForDelete = "No Configuration Selected";
String selectedDistanceForDelete = "N/A";
String selectedAngleForDelete = "N/A";
String selectedSpeedForDelete = "N/A";
int selectedConfigIndexForDelete = -1;

// SPIFFS file for configurations
const char* CONFIG_FILE = "/configs.json";

// Process simulation state
String processStatus = "Ready";
unsigned long processStartTime = 0;
unsigned long processPauseTime = 0;
unsigned long processDurationSeconds = 0;
unsigned long remainingTimeSeconds = 0;
bool supplyIsOn = false;
float simulatedVoltageReading = 0.0;
float simulatedCurrentReading = 0.0;
unsigned long lastRealTimeUpdate = 0; // Unused, kept as per original structure

// Convert all configurations to JSON string
String serializeAllConfigsToJson(const std::vector<Configuration>& configs) {
  DynamicJsonDocument doc(1024);
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
  DynamicJsonDocument doc(1024);
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

// Display a simple message box
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
                    <p class="mt-1 block w-full bg-gray-100 border border-gray-300 rounded-md shadow-sm p-2 text-gray-800">%CURRENT_DISTANCE%</p>
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
      currentSpeed = configurations[index].speed;
      processDurationSeconds = currentAngle.toInt(); // Angle maps to process duration
      remainingTimeSeconds = processDurationSeconds;
      processStatus = "Ready";
      supplyIsOn = false;
      simulatedVoltageReading = 0.0;
      simulatedCurrentReading = 0.0;
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
        const configurationsData = [ %JS_CONFIGURATIONS_ARRAY% ];
        function selectConfig(index) {
            if (selectedIndex !== -1) {
                const prevSelected = document.getElementById('config-' + selectedIndex);
                if (prevSelected) prevSelected.classList.remove('selected-config');
            }
            const newSelected = document.getElementById('config-' + index);
            if (newSelected) newSelected.classList.add('selected-config');
            selectedIndex = index;
            const config = configurationsData[index];
            document.getElementById('displayConfigName').textContent = config.name;
            document.getElementById('displayDistance').textContent = config.distance + 'cm';
            document.getElementById('displayAngle').textContent = config.angle;
            document.getElementById('displaySpeed').textContent = config.speed;
            const deleteButton = document.getElementById('deleteButton');
            deleteButton.disabled = false;
            deleteButton.classList.remove('opacity-50', 'cursor-not-allowed');
        }
        function confirmDelete() {
            if (selectedIndex !== -1) {
                if (confirm('Are you sure you want to delete "' + configurationsData[selectedIndex].name + '"?')) {
                    window.location.href = '/delete-selected-config?index=' + selectedIndex;
                }
            }
        }
        window.onload = function() {
            const initialConfigName = "%SELECTED_CONFIG_NAME_FOR_DELETE%";
            if (initialConfigName !== "No Configuration Selected") {
                    document.getElementById('displayConfigName').textContent = initialConfigName;
                    document.getElementById('displayDistance').textContent = "%SELECTED_DISTANCE_FOR_DELETE%" + ("%SELECTED_DISTANCE_FOR_DELETE%" === "N/A" ? "" : "cm");
                    document.getElementById('displayAngle').textContent = "%SELECTED_ANGLE_FOR_DELETE%";
                    document.getElementById('displaySpeed').textContent = "%SELECTED_SPEED_FOR_DELETE%";
            }
        };
    </script>
</body>
</html>
  )rawliteral";

  String jsConfigArray = "";
  if (!configurations.empty()) {
    for (size_t i = 0; i < configurations.size(); ++i) {
      jsConfigArray += "{ name: \"" + configurations[i].name + "\", ";
      jsConfigArray += "distance: \"" + configurations[i].distance + "\", ";
      jsConfigArray += "angle: \"" + configurations[i].angle + "\", ";
      jsConfigArray += "speed: \"" + configurations[i].speed + "\" }";
      if (i < configurations.size() - 1) jsConfigArray += ", ";
    }
  }

  html.replace("%CONFIG_LIST_HTML%", configListHtml);
  html.replace("%JS_CONFIGURATIONS_ARRAY%", jsConfigArray);
  html.replace("%SELECTED_CONFIG_NAME_FOR_DELETE%", selectedConfigNameForDelete);
  html.replace("%SELECTED_DISTANCE_FOR_DELETE%", selectedDistanceForDelete);
  html.replace("%SELECTED_ANGLE_FOR_DELETE%", selectedAngleForDelete);
  html.replace("%SELECTED_SPEED_FOR_DELETE%", selectedConfigNameForDelete == "No Configuration Selected" ? "N/A" : selectedSpeedForDelete);
  server.send(200, "text/html", html);
}

// Delete selected configuration
void handleDeleteSelectedConfig() {
  if (!isLoggedIn || !isAdmin) {
    server.sendHeader("Location", "/home");
    server.send(302, "text/plain", "");
    return;
  }

  if (server.hasArg("index")) {
    int index = server.arg("index").toInt();
    if (index >= 0 && index < configurations.size()) {
      configurations.erase(configurations.begin() + index);
      saveConfigurationsToSPIFFS();
      selectedConfigNameForDelete = "No Configuration Selected";
      selectedDistanceForDelete = "N/A";
      selectedAngleForDelete = "N/A";
      selectedSpeedForDelete = "N/A";
      selectedConfigIndexForDelete = -1;
    }
  }
  handleDeleteConfigPage();
}

// Handle Service Mode page
void handleServiceMode() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Service Mode</title>
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
        <div class="bg-white rounded-lg shadow-xl p-8 text-center max-w-2xl w-full">
            <h2 class="text-2xl font-bold text-gray-900 mb-4">Service Mode Functions</h2>
            <p class="text-gray-700 mb-6"> This section is for maintenance and advanced settings. </p>
            <div class="flex flex-col space-y-4">
                <a href="/service-mode/test-parts" class="bg-blue-600 hover:bg-blue-700 text-white font-semibold py-3 px-6 rounded-md shadow-md transition duration-300 ease-in-out text-center"> Test Individual Parts </a>
            </div>
        </div>
    </main>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", html);
}

// Handle Test Individual Parts page
void handleTestPartsPage() {
  if (!isLoggedIn || !isAdmin) {
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
    <title>Test Individual Parts</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; }
        input[type="range"]::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 20px; height: 20px; background: #2563eb; cursor: pointer; border-radius: 50%; box-shadow: 0 0 2px rgba(0,0,0,0.5); }
        input[type="range"]::-moz-range-thumb { width: 20px; height: 20px; background: #2563eb; cursor: pointer; border-radius: 50%; box-shadow: 0 0 2px rgba(0,0,0,0.5); }
    </style>
</head>
<body class="min-h-screen flex flex-col">
    <header class="w-full bg-white shadow-sm p-4">
        <nav class="container mx-auto flex items-center justify-between">
            <div class="flex items-center space-x-4">
                <a href="/service-mode" class="text-gray-600 hover:text-gray-900">
                    <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M10.5 19.5L3 12m0 0l7.5-7.5M3 12h18" /> </svg>
                </a>
                <h1 class="text-xl font-semibold text-gray-800">Test Individual Parts</h1>
            </div>
            <a href="/home" class="text-gray-600 hover:text-red-600">
                <svg xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24" stroke-width="2.5" stroke="currentColor" class="w-6 h-6"> <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" /> </svg>
            </a>
        </nav>
    </header>
    <main class="flex-grow flex items-center justify-center p-4">
        <div class="bg-white rounded-lg shadow-xl p-8 flex flex-col md:flex-row items-start space-y-8 md:space-y-0 md:space-x-12 max-w-4xl w-full">
            <div class="flex-1 w-full space-y-4 p-4 border border-gray-200 rounded-md shadow-sm">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Stepper Motor (Wheel)</h2>
                <form id="stepperForm" class="space-y-4">
                    <div>
                        <label for="stepperSteps" class="block text-sm font-medium text-gray-700 mb-1">Steps (e.g., 200 for full rotation)</label>
                        <input type="number" id="stepperSteps" name="steps" value="200" min="1" class="mt-1 block w-full border-gray-300 rounded-md shadow-sm p-2">
                    </div>
                    <div class="flex space-x-4">
                        <button type="button" onclick="sendStepperCommand('forward')" class="flex-1 bg-green-600 hover:bg-green-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out"> Move Forward </button>
                        <button type="button" onclick="sendStepperCommand('backward')" class="flex-1 bg-red-600 hover:bg-red-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out"> Move Backward </button>
                    </div>
                </form>
                <p id="stepperStatus" class="text-sm text-gray-600 mt-2"></p>
            </div>
            <div class="flex-1 w-full space-y-4 p-4 border border-gray-200 rounded-md shadow-sm">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Servo Motor (Arm)</h2>
                <form id="servoForm" class="space-y-4">
                    <div>
                        <label for="servoAngle" class="block text-sm font-medium text-gray-700 mb-1">Angle (0-180 degrees)</label>
                        <input type="range" id="servoAngle" name="angle" min="0" max="180" value="90" oninput="document.getElementById('servoAngleValue').innerText = this.value + '°'" class="mt-1 block w-full">
                        <p class="text-center text-gray-600">Current Angle: <span id="servoAngleValue">90°</span></p>
                    </div>
                    <button type="button" onclick="sendServoCommand()" class="w-full bg-blue-600 hover:bg-blue-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out"> Set Angle </button>
                </form>
                <p id="servoStatus" class="text-sm text-gray-600 mt-2"></p>
            </div>
            <div class="flex-1 w-full space-y-4 p-4 border border-gray-200 rounded-md shadow-sm">
                <h2 class="text-xl font-semibold text-gray-800 mb-2">Load Cell</h2>
                <p class="text-gray-700 text-lg">Current Reading: <span id="loadCellReading" class="font-bold text-blue-700">N/A</span></p>
                <button type="button" onclick="getLoadCellReading()" class="w-full bg-purple-600 hover:bg-purple-700 text-white font-semibold py-2 px-4 rounded-md shadow-md transition duration-300 ease-in-out"> Refresh Reading </button>
                <p id="loadCellStatus" class="text-sm text-gray-600 mt-2"></p>
            </div>
        </div>
    </main>
    <script>
        async function sendCommand(url, method, body = null) {
            try {
                const options = { method: method };
                if (body) { options.headers = { 'Content-Type': 'application/x-www-form-urlencoded' }; options.body = new URLSearchParams(body).toString(); }
                const response = await fetch(url, options);
                return await response.json();
            } catch (error) {
                return { status: 'error', message: error.message };
            }
        }
        async function sendStepperCommand(direction) {
            const steps = document.getElementById('stepperSteps').value;
            const statusElement = document.getElementById('stepperStatus');
            statusElement.textContent = 'Moving...';
            const result = await sendCommand('/service-mode/stepper-control', 'POST', { steps: steps, direction: direction });
            statusElement.textContent = result.message || 'Command sent.';
            statusElement.style.color = result.status === 'success' ? 'green' : 'red';
        }
        async function sendServoCommand() {
            const angle = document.getElementById('servoAngle').value;
            const statusElement = document.getElementById('servoStatus');
            statusElement.textContent = 'Setting angle...';
            const result = await sendCommand('/service-mode/servo-control', 'POST', { angle: angle });
            statusElement.textContent = result.message || 'Command sent.';
            statusElement.style.color = result.status === 'success' ? 'green' : 'red';
        }
        async function getLoadCellReading() {
            const readingElement = document.getElementById('loadCellReading');
            const statusElement = document.getElementById('loadCellStatus');
            statusElement.textContent = 'Reading...';
            const result = await sendCommand('/service-mode/loadcell-read', 'GET');
            if (result.status === 'success' && result.value !== undefined) {
                readingElement.textContent = result.value + ' units';
                statusElement.textContent = 'Reading refreshed.';
                statusElement.style.color = 'green';
            } else {
                readingElement.textContent = 'Error';
                statusElement.textContent = result.message || 'Failed to read load cell.';
                statusElement.style.color = 'red';
            }
        }
        setInterval(getLoadCellReading, 3000);
        window.onload = getLoadCellReading;
    </script>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", html);
}

// Handle Stepper motor control
void handleStepperControl() {
  if (!isLoggedIn || !isAdmin) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  String steps = server.arg("steps");
  String direction = server.arg("direction");
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Stepper command sent: " + direction + " " + steps + " steps\"}");
}

// Handle Servo motor control
void handleServoControl() {
  if (!isLoggedIn || !isAdmin) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  String angle = server.arg("angle");
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Servo angle set to " + angle + "°\"}");
}

// Get Load Cell reading
void handleGetLoadCellReading() {
  if (!isLoggedIn || !isAdmin) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  float simulatedReading = random(100, 1000) / 10.0;
  String responseJson = "{\"status\":\"success\",\"value\":" + String(simulatedReading) + "}";
  server.send(200, "application/json", responseJson);
}

// Start process
void handleProcessStart() {
  if (!isLoggedIn) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  if (processStatus == "Running") {
    server.send(200, "application/json", "{\"status\":\"info\",\"message\":\"Process is already running.\"}");
    return;
  }
  if (currentConfigName == "No Configuration Loaded" || currentAngle == "N/A") {
      server.send(200, "application/json", "{\"status\":\"error\",\"message\":\"No configuration loaded.\"}");
      return;
  }
  if (processStatus == "Paused") {
    processStartTime = millis() - processPauseTime;
  } else {
    processStartTime = millis();
    processDurationSeconds = currentAngle.toInt(); // Angle maps to process duration
    remainingTimeSeconds = processDurationSeconds;
  }
  processStatus = "Running";
  supplyIsOn = true;
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Process started.\"");
}

// Pause process
void handleProcessPause() {
  if (!isLoggedIn) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  if (processStatus != "Running") {
    server.send(200, "application/json", "{\"status\":\"info\",\"message\":\"Process is not running to pause.\"}");
    return;
  }
  processPauseTime = millis() - processStartTime;
  processStatus = "Paused";
  supplyIsOn = false;
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Process paused.\"}");
}

// Stop process
void handleProcessStop() {
  if (!isLoggedIn) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }
  if (processStatus == "Ready" || processStatus == "Stopped" || processStatus == "Completed") {
    server.send(200, "application/json", "{\"status\":\"info\",\"message\":\"Process is already stopped or not started.\"}");
    return;
  }
  processStatus = "Stopped";
  supplyIsOn = false;
  remainingTimeSeconds = 0;
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Process stopped.\"}");
}

// Get real-time process data
void handleProcessRealtime() {
  if (!isLoggedIn) {
    server.send(403, "application/json", "{\"status\":\"error\",\"message\":\"Unauthorized\"}");
    return;
  }

  if (processStatus == "Running") {
    unsigned long elapsedTime = (millis() - processStartTime) / 1000;
    if (elapsedTime >= processDurationSeconds) {
      processStatus = "Completed";
      supplyIsOn = false;
      remainingTimeSeconds = 0;
    } else {
      remainingTimeSeconds = processDurationSeconds - elapsedTime;
      simulatedVoltageReading = currentSpeed.toFloat();
      simulatedCurrentReading = random(100, 200) / 100.0;
    }
  } else if (processStatus == "Stopped" || processStatus == "Completed" || processStatus == "Ready") {
      supplyIsOn = false;
      simulatedVoltageReading = 0.0;
      simulatedCurrentReading = 0.0;
      remainingTimeSeconds = 0;
  }

  String responseJson = "{";
  responseJson += "\"status\":\"" + processStatus + "\",";
  responseJson += "\"supplyOn\":" + String(supplyIsOn ? "true" : "false") + ",";
  responseJson += "\"voltage\":" + String(simulatedVoltageReading, 2) + ",";
  responseJson += "\"current\":" + String(simulatedCurrentReading, 2) + ",";
  responseJson += "\"remainingTime\":" + String(remainingTimeSeconds);
  responseJson += "}";
  server.send(200, "application/json", responseJson);
}

// Handle Process Mode page display
void handleProcessPage() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Process Mode</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <style>
        body { font-family: 'Inter', sans-serif; background-color: #f0f4f8; }
        .section-box { background-color: #ffffff; border-radius: 0.5rem; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06); padding: 1.5rem; }
        .value-display { background-color: #f3f4f6; border: 1px solid #d1d5db; border-radius: 0.375rem; padding: 0.5rem; color: #1f2937; font-weight: 500; }
        .control-button { font-weight: 600; padding: 0.75rem 2rem; border-radius: 0.5rem; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06); transition: all 0.3s ease-in-out; text-align: center; }
        .control-button.start { background-color: #10b981; color: white; }
        .control-button.start:hover { background-color: #059669; }
        .control-button.pause { background-color: #f59e0b; color: white; }
        .control-button.pause:hover { background-color: #d97706; }
        .control-button.stop { background-color: #ef4444; color: white; }
        .control-button.stop:hover { background-color: #dc2626; }
    </style>
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
        <div class="w-full max-w-6xl mx-auto flex flex-col gap-8">
            <div class="flex flex-col md:flex-row gap-8">
                <div class="section-box flex-1 space-y-4">
                    <h2 class="text-xl font-bold text-gray-800 mb-4">User-Defined Values</h2>
                    <div>
                        <label class="block text-sm font-medium text-gray-700 mb-1">Name</label>
                        <p class="value-display" id="displayConfigName">%CURRENT_CONFIG_NAME%</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700 mb-1">Distance</label>
                        <p class="value-display" id="displayDistance">%CURRENT_DISTANCE%mm</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700 mb-1">Angle</label>
                        <p class="value-display" id="displayAngle">%CURRENT_ANGLE%</p>
                    </div>
                    <div>
                        <label class="block text-sm font-medium text-gray-700 mb-1">Speed</label>
                        <p class="value-display" id="displaySpeed">%CURRENT_SPEED%</p>
                    </div>
                </div>
                <div class="section-box flex-1 flex items-center justify-center bg-gray-50 border border-gray-200">
                    <p class="text-gray-400 text-lg">Graph/Visual Area</p>
                </div>
                <div class="section-box flex-1 space-y-4">
                    <h2 class="text-xl font-bold text-gray-800 mb-4">Real-Time Values</h2>
                    <div>
                        <label class="block text-sm font-medium text-gray-700 mb-1">Status</label>
                        <p class="value-display" id="realtimeStatus">Ready</p>
                    </div>
                    <div class="text-center mt-6">
                        <label class="block text-sm font-medium text-gray-700 mb-1">Remaining Time</label>
                        <p class="text-4xl font-bold text-blue-700" id="remainingTime">00 : 00</p>
                        <p class="text-xs text-gray-500">MM : SS</p>
                    </div>
                </div>
            </div>
            <div class="w-full flex justify-center space-x-6 mt-8">
                <button onclick="sendProcessCommand('start')" class="control-button start">START</button>
                <button onclick="sendProcessCommand('pause')" class="control-button pause">PAUSE</button>
                <button onclick="sendProcessCommand('stop')" class="control-button stop">STOP</button>
            </div>
        </div>
    </main>
    <script>
        async function sendCommand(url, method, body = null) {
            try {
                const options = { method: method };
                if (body) { options.headers = { 'Content-Type': 'application/x-www-form-urlencoded' }; options.body = new URLSearchParams(body).toString(); }
                const response = await fetch(url, options);
                return await response.json();
            } catch (error) {
                return { status: 'error', message: error.message };
            }
        }
        async function sendProcessCommand(command) {
            const result = await sendCommand(`/process/${command}`, 'POST');
            if (result.status === 'error') { alert(`Error: ${result.message}`); }
            updateRealtimeValues();
        }
        async function updateRealtimeValues() {
            const result = await sendCommand('/process/realtime', 'GET');
            if (result.status === 'success') {
                document.getElementById('realtimeStatus').textContent = result.status;
                const minutes = Math.floor(result.remainingTime / 60).toString().padStart(2, '0');
                const seconds = (result.remainingTime % 60).toString().padStart(2, '0');
                document.getElementById('remainingTime').textContent = `${minutes} : ${seconds}`;
            } else {
                document.getElementById('realtimeStatus').textContent = 'Error';
                document.getElementById('remainingTime').textContent = '00 : 00';
            }
        }
        setInterval(updateRealtimeValues, 1000);
        window.onload = updateRealtimeValues;
    </script>
</body>
</html>
  )rawliteral";
  html.replace("%CURRENT_CONFIG_NAME%", currentConfigName);
  html.replace("%CURRENT_ANGLE%", currentAngle);
  html.replace("%CURRENT_SPEED%", currentSpeed);
  html.replace("%CURRENT_DISTANCE%", currentDistance);
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed.");
    while(true) delay(100);
  }
  loadConfigurationsFromSPIFFS();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(); // Newline after dots
  Serial.print("Connected to WiFi with IP: ");
  Serial.println(WiFi.localIP()); // Print IP address

  if (MDNS.begin("flexural-machine")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", handleLogout);
  server.on("/home", handleHomeRouter);
  server.on("/page1", handlePage1);
  server.on("/page2", HTTP_GET, handlePage2);
  server.on("/page2", HTTP_POST, handlePage2);
  server.on("/load-selected-config", handleLoadSelectedConfig);
  server.on("/delete-config-page", handleDeleteConfigPage);
  server.on("/delete-selected-config", handleDeleteSelectedConfig);
  server.on("/service-mode", handleServiceMode);
  server.on("/service-mode/test-parts", handleTestPartsPage);
  server.on("/service-mode/stepper-control", HTTP_POST, handleStepperControl);
  server.on("/service-mode/servo-control", HTTP_POST, handleServoControl);
  server.on("/service-mode/loadcell-read", HTTP_GET, handleGetLoadCellReading);
  server.on("/process", handleProcessPage);
  server.on("/process/start", HTTP_POST, handleProcessStart);
  server.on("/process/pause", HTTP_POST, handleProcessPause);
  server.on("/process/stop", HTTP_POST, handleProcessStop);
  server.on("/process/realtime", HTTP_GET, handleProcessRealtime);

  server.begin();
}

void loop() {
  server.handleClient();
}
