#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include "HX711.h"
// #include <ESP32Servo.h> // Uncomment if you add servo functionality

// --- Load cell pins and calibration ---
#define DT 3
#define SCK 2
HX711 scale;
float calibration_factor = -475.31;

// --- Web server setup ---
WebServer server(80);
bool mDNSStarted = false;

// --- Physical Reset Button ---
#define RESET_BUTTON_PIN 4
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

// For storing readings history
String forceHistory = "";
int readingCounter = 0; // Global counter for force readings

// --- Stepper Motor Pin Definitions ---
const int STEP_PIN = 19;
const int DIR_PIN = 18;
const int ENABLE_PIN = 5; // ENA- on TB6600

// --- Stepper Motion Parameters ---
const int stepsPerRevolution = 400;           // 400 steps per full rotation (half-step mode)
float distancePerStep = 9.11 / 400.0;         // ~0.02278 cm per step (based on 2.9 cm wheel)

// // --- Servo Pin Definition (EXAMPLE - Add your servo code) ---
// #define SERVO_PIN 17 // Example pin for servo
// // Servo myServo; // Uncomment if you add servo functionality
// // Define servo angles if using a servo for pressure application
// // const int SERVO_APPLY_PRESSURE_ANGLE = 90; // Example angle
// // const int SERVO_RETRACT_ANGLE = 0;         // Example angle

// --- Stepper Control Functions ---
void moveStepper(float cmToMove) {
  // Determine direction based on positive/negative cmToMove
  // Assuming positive cmToMove is "pushing forward" (DIR_PIN LOW)
  // Assuming negative cmToMove is "pulling back" (DIR_PIN HIGH)
  int direction = (cmToMove >= 0) ? LOW : HIGH; // LOW for forward, HIGH for backward
  digitalWrite(DIR_PIN, direction);

  long steps = abs(cmToMove) / distancePerStep;
  Serial.print("Moving stepper ");
  Serial.print(cmToMove);
  Serial.print(" cm (");
  Serial.print(steps);
  Serial.println(" steps)");

  // ENABLE motor (inverted logic: HIGH = enabled)
  digitalWrite(ENABLE_PIN, HIGH);
  delay(10); // Small delay for enable to settle

  for (long i = 0; i < steps; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(500); // Adjust for desired speed
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(500); // Adjust for desired speed
  }

  // DISABLE motor (inverted logic: LOW = disabled)
  digitalWrite(ENABLE_PIN, LOW);
  Serial.println("Stepper motor move complete. Motor disabled.");
}

// NEW FUNCTION: Moves the tube only, no measurement
void moveTubeOnly(float distance) {
  moveStepper(distance);
  // No force measurement or servo action here
  Serial.println("Tube moved. Ready for next action.");
}

// Function to perform the full sequence: move stepper, apply pressure (servo - future), read force
// This function is kept for scenarios where you want both actions on one button press
void moveTubeAndMeasure(float distance) {
    // 1. Move the stepper motor
    moveStepper(distance);
    delay(500); // Small delay after movement for mechanical settling

    // 2. (Future) Move servo to apply pressure - UNCOMMENT AND CONFIGURE WHEN ADDING SERVO
    // Serial.println("Moving servo to apply pressure...");
    // myServo.write(SERVO_APPLY_PRESSURE_ANGLE);
    // delay(1500); // Wait for servo to move and pressure to stabilize before reading

    // 3. Read force from load cell
    sendCurrentForce();

    // 4. (Future) Retract servo - UNCOMMENT AND CONFIGURE WHEN ADDING SERVO
    // Serial.println("Retracting servo...");
    // myServo.write(SERVO_RETRACT_ANGLE);
    // delay(1000); // Wait for servo to retract
}


// Function to send current force data using HX711
void sendCurrentForce() {
  if (scale.is_ready()) {
    float grams = scale.get_units(10);
    float newtons = grams * 0.00980665; // Conversion to Newtons

    readingCounter++; // Increment the counter for each new reading
    forceHistory += String(readingCounter) + ": " + String(newtons, 3) + " N\n"; // Use the counter here
    Serial.println("Measured Force: " + String(newtons, 3) + " N");
  } else {
    Serial.println("HX711 not ready.");
    readingCounter++; // Increment even on error to keep numbering consistent
    forceHistory += String(readingCounter) + ": HX711 not ready\n";
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP32 Load Cell & Stepper Setup ---");

  // Load cell setup
  scale.begin(DT, SCK);
  scale.set_scale(calibration_factor);
  scale.tare();
  Serial.println("Scale ready. Place your weight.");

  // Stepper Motor Setup
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, LOW); // Initially disabled (inverted logic)
  Serial.println("Stepper Motor Initialized.");

  // // Servo Setup (EXAMPLE - Uncomment and configure when adding servo)
  // // myServo.attach(SERVO_PIN);
  // // myServo.write(SERVO_RETRACT_ANGLE); // Ensure servo starts in a safe position
  // // Serial.println("Servo Motor Initialized.");

  // Reset button
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  WiFiManager wm;
  wm.setConfigPortalTimeout(40);
  bool connectedToRouter = wm.autoConnect("ESP32-Setup", "12345678");

  if (connectedToRouter) {
    Serial.println("✅ Connected to Wi-Fi!");
    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("esp32")) {
      Serial.println("🌐 mDNS responder started at http://esp32.local");
      mDNSStarted = true;
    } else {
      Serial.println("❌ mDNS failed");
    }
  } else {
    Serial.println("❌ Wi-Fi failed. Starting fallback AP...");
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-DirectConnect", "12345678");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // --- Web Server Pages ---
  server.on("/", []() {
    String accessUrl = (WiFi.getMode() == WIFI_AP) ? "http://192.168.4.1" :
                        (mDNSStarted ? "http://esp32.local" : "http://" + WiFi.localIP().toString());

    String html = "<!DOCTYPE html><html><head><title>ESP32 Load Cell & Stepper</title><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
    html += "body { font-family: sans-serif; background:#f0f2f5; padding:20px; text-align:center; color:#333; }"; // Reduced padding
    html += "h2, h3 { color: #222; margin-bottom: 15px; }";
    html += "button { padding:12px 25px; margin:8px; border:none; border-radius:6px; background:#007bff; color:white; font-size:16px; cursor:pointer; transition: background 0.3s ease; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }"; // Improved button styles
    html += "button:hover { background:#0056b3; transform: translateY(-1px); box-shadow: 0 4px 8px rgba(0,0,0,0.2); }"; // Hover effects
    html += ".action-group { margin: 25px auto; border: 1px solid #ddd; padding: 20px; border-radius: 10px; background: #fff; max-width: 500px; box-shadow: 0 4px 10px rgba(0,0,0,0.05); }"; // Group styling
    html += ".action-group h3 { margin-top: 0; padding-bottom: 10px; border-bottom: 1px solid #eee; }";
    html += "pre { background:#e9ecef; padding:15px; border-radius:8px; max-width:600px; margin:20px auto; text-align:left; white-space: pre-wrap; word-wrap: break-word; font-size: 0.9em; line-height: 1.6; color: #444; border: 1px solid #dee2e6; overflow-y: auto; max-height: 250px; }"; // History box improvements
    html += "p { font-size: 0.9em; color: #555; }";
    html += "</style></head><body>";
    html += "<h2>ESP32 Tube Pressure Tester</h2>";

    html += "<div class='action-group'>";
    html += "<h3>Tube Positioning</h3>";
    html += "<p>Move the tube to a specific distance without taking a force reading.</p>";
    html += "<a href='/move10cm_only'><button>Move 10cm</button></a>"; // Modified: Calls new endpoint
    html += "<a href='/move40cm_only'><button>Move 40cm</button></a>"; // Modified: Calls new endpoint
    html += "<a href='/move60cm_only'><button>Move 60cm</button></a>"; // Modified: Calls new endpoint
    html += "</div>";

    // // Optional: Add a section for "Move and Measure" if you still want that combined action
    // html += "<div class='action-group'>";
    // html += "<h3>Move & Measure</h3>";
    // html += "<p>Move the tube and then automatically take a force reading.</p>";
    // html += "<a href='/move10cm_and_measure'><button style='background:#6c757d;'>Move 10cm & Measure</button></a>";
    // html += "<a href='/move40cm_and_measure'><button style='background:#6c757d;'>Move 40cm & Measure</button></a>";
    // html += "<a href='/move60cm_and_measure'><button style='background:#6c757d;'>Move 60cm & Measure</button></a>";
    // html += "</div>";

    html += "<div class='action-group'>";
    html += "<h3>Force Measurement</h3>";
    html += "<p>Take a force reading at the current tube position.</p>";
    html += "<a href='/read_force'><button style='background:#28a745;'>Measure Current Force</button></a>"; // This button now explicitly measures
    html += "</div>";

    html += "<div class='action-group'>";
    html += "<h3>Force Data Management</h3>";
    html += "<a href='/reset_force_data'><button style='background:#ffc107;color:#000;'>Reset All Force Data</button></a>";
    html += "</div>";

    html += "<div class='action-group'>";
    html += "<h3>System & Wi-Fi Configuration</h3>";
    html += "<a href='/reset'><button style='background:#f44336;'>Reset Wi-Fi Settings & Reboot</button></a>";
    html += "</div>";

    html += "<h3>Force Readings History</h3>";
    html += "<pre>" + forceHistory + "</pre>";
    html += "<p>Access URL: <strong>" + accessUrl + "</strong></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // --- New Stepper Motor Only Endpoints ---
  server.on("/move10cm_only", []() {
    Serial.println("Web request: Moving 10cm (no measurement)...");
    moveTubeOnly(10.0); // Call the new function that only moves the stepper
    server.sendHeader("Location", "/", true); // Redirect back to the root page
    server.send(302, "text/plain", "");      // Send the redirect response
  });

  server.on("/move40cm_only", []() {
    Serial.println("Web request: Moving 40cm (no measurement)...");
    moveTubeOnly(40.0); // Call the new function that only moves the stepper
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.on("/move60cm_only", []() {
    Serial.println("Web request: Moving 60cm (no measurement)...");
    moveTubeOnly(60.0); // Call the new function that only moves the stepper
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  // // Optional: Endpoints for combined Move and Measure
  // server.on("/move10cm_and_measure", []() {
  //   Serial.println("Web request: Moving 10cm AND measuring...");
  //   moveTubeAndMeasure(10.0); // Call the combined function
  //   server.sendHeader("Location", "/", true);
  //   server.send(302, "text/plain", "");
  // });
  // server.on("/move40cm_and_measure", []() {
  //   Serial.println("Web request: Moving 40cm AND measuring...");
  //   moveTubeAndMeasure(40.0);
  //   server.sendHeader("Location", "/", true);
  //   server.send(302, "text/plain", "");
  // });
  // server.on("/move60cm_and_measure", []() {
  //   Serial.println("Web request: Moving 60cm AND measuring...");
  //   moveTubeAndMeasure(60.0);
  //   server.sendHeader("Location", "/", true);
  //   server.send(302, "text/plain", "");
  // });


  server.on("/read_force", []() {
    Serial.println("Web request: Measuring current force only...");
    sendCurrentForce(); // This is now the dedicated measure button
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.on("/reset_force_data", []() {
    forceHistory = "";
    readingCounter = 0; // Reset counter when data is cleared
    Serial.println("Force data reset.");
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.on("/reset", []() {
    server.send(200, "text/html", "<h3>Resetting Wi-Fi and rebooting...</h3>");
    delay(1000);
    WiFi.disconnect(true, true);
    ESP.restart();
  });

  server.begin();
  Serial.println("📡 Web server started.");
}

void loop() {
  server.handleClient();

  // Handle physical reset button
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
      Serial.println("Reset button pressed. Hold for 3 seconds for factory reset...");
    } else if (millis() - buttonPressTime > 3000) {
      Serial.println("Long press detected — Performing factory reset...");
      WiFiManager wm;
      wm.resetSettings();
      delay(500);
      ESP.restart();
    }
  } else {
    buttonPressed = false;
  }
}