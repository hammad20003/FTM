#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include "HX711.h" // Reverted to HX711.h
#include <ESP32Servo.h>
#include <WebSocketsServer.h> // Added for real-time data streaming

// --- Load cell pins and calibration ---
#define DT 3
#define SCK 2
HX711 scale; // Reverted to HX711 instance named 'scale'
float calibration_factor = -475.31; // Using your original calibration factor

// --- Web server setup ---
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81); // WebSocket server on port 81
bool mDNSStarted = false;

// --- Physical Reset Button ---
#define RESET_BUTTON_PIN 4
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

// For storing readings history (for the old "Force Readings History" section)
String forceHistory = "";
int readingCounter = 0; // Global counter for force readings

// --- Stepper Motor Pin Definitions ---
const int STEP_PIN = 19;
const int DIR_PIN = 18;
const int ENABLE_PIN = 5; // ENA- on TB6600

// --- Stepper Motion Parameters ---
const int stepsPerRevolution = 400;          // 400 steps per full rotation (half-step mode)
float distancePerStep = 9.11 / 400.0;        // ~0.02278 cm per step (based on 2.9 cm wheel)

// --- Servo Motor Definitions ---
#define SERVO_PIN 7 // IMPORTANT: Defined as 7 as per your request
Servo myServo;
// Define angles for servo operation (adjust these based on your setup)
const int SERVO_RETRACT_ANGLE = 0;   // Angle where servo is retracted (not pushing)
const int SERVO_APPLY_PRESSURE_ANGLE = 90; // Example: Angle where servo pushes load cell

// --- Flexural Test Specific Variables ---
bool testRunning = false;
int currentServoAngle = SERVO_RETRACT_ANGLE; // Start from retracted position
const int SERVO_TEST_END_ANGLE = 60; // Max angle for the flexural test (based on your 60-degree reference)
const int SERVO_TEST_STEP_ANGLE = 1; // Increment angle for each test step
unsigned long lastDataSendTime = 0;
const unsigned long DATA_SEND_INTERVAL = 50; // Milliseconds between sending data points

// --- Stepper Control Functions ---
void moveStepper(float cmToMove) {
  int direction = (cmToMove >= 0) ? LOW : HIGH;
  digitalWrite(DIR_PIN, direction);

  long steps = abs(cmToMove) / distancePerStep;
  Serial.print("Moving stepper ");
  Serial.print(cmToMove);
  Serial.print(" cm (");
  Serial.print(steps);
  Serial.println(" steps)");

  digitalWrite(ENABLE_PIN, HIGH);
  delay(10); // Small delay to ensure enable is active before pulsing

  for (long i = 0; i < steps; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(500); // Adjust for stepper speed
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(500); // Adjust for stepper speed
  }

  digitalWrite(ENABLE_PIN, LOW);
  Serial.println("Stepper motor move complete. Motor disabled.");
}

// Function to move the tube only, no measurement
void moveTubeOnly(float distance) {
  moveStepper(distance);
  Serial.println("Tube moved. Ready for next action.");
}

// Function to send a single current force data reading using HX711
void sendCurrentForce() {
  if (scale.is_ready()) { // Using is_ready() from HX711.h
    float grams = scale.get_units(10); // Using get_units(10) from HX711.h
    float newtons = grams * 0.00980665; // Convert grams to Newtons

    readingCounter++;
    forceHistory += String(readingCounter) + ": " + String(newtons, 3) + " N\n";
    Serial.println("Measured Force: " + String(newtons, 3) + " N");
  } else {
    Serial.println("HX711 not ready.");
    // Optionally, you could still add a placeholder to history if no data
    // readingCounter++;
    // forceHistory += String(readingCounter) + ": HX711 not ready\n";
  }
}

// Function to move the servo to a specific angle
void moveServo(int angle) {
  Serial.print("Moving servo to ");
  Serial.print(angle);
  Serial.println(" degrees.");
  myServo.write(angle);
  delay(500); // Allow time for servo to reach position and stabilize
}

// WebSocket event handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Disconnected!\n", num);
            break;
        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(num);
            Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
            // Send initial status to the newly connected client
            webSocket.sendTXT(num, "{\"type\":\"status\",\"message\":\"Connected\"}");
        }
            break;
        case WStype_TEXT:
            Serial.printf("[%u] get Text: %s\n", num, payload);
            if (strcmp((char*)payload, "START_TEST") == 0) {
                if (!testRunning) {
                    Serial.println("Starting flexural test...");
                    testRunning = true;
                    currentServoAngle = SERVO_RETRACT_ANGLE; // Reset servo position for test start
                    myServo.write(currentServoAngle); // Move servo to start position
                    webSocket.broadcastTXT("{\"type\":\"status\",\"message\":\"Test Running\"}");
                }
            } else if (strcmp((char*)payload, "STOP_TEST") == 0) {
                if (testRunning) {
                    Serial.println("Stopping flexural test...");
                    testRunning = false;
                    webSocket.broadcastTXT("{\"type\":\"status\",\"message\":\"Test Stopped\"}");
                }
            }
            break;
        case WStype_BIN:
        case WStype_ERROR:
        // Removed WStype_FRAGMENT_NFA as it's not a valid type in this library version
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT_FIN:
            break;
    }
}

// HTML content stored in PROGMEM (Flash Memory)
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Tube Pressure Tester</title>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <!-- Tailwind CSS CDN -->
    <script src="https://cdn.tailwindcss.com"></script>
    <!-- Inter font from Google Fonts -->
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700&display=swap" rel="stylesheet">
    <!-- Chart.js for plotting graphs -->
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: 'Inter', sans-serif; background:#f0f2f5; padding:20px; text-align:center; color:#333; display: flex; flex-direction: column; min-height: 100vh; }
        .main-layout { display: grid; grid-template-columns: 1fr 2fr 1fr; grid-template-rows: auto auto auto; gap: 20px; flex-grow: 1; max-width: 1200px; margin: 0 auto; padding-bottom: 20px; }
        .grid-area { padding: 15px; border-radius: 10px; background: #fff; box-shadow: 0 4px 10px rgba(0,0,0,0.05); }
        .left-column { grid-column: 1 / 2; grid-row: 1 / 2; display: flex; flex-direction: column; gap: 10px; align-items: flex-start; }
        .center-data-display { grid-column: 2 / 3; grid-row: 1 / 3; display: flex; flex-direction: column; justify-content: flex-start; align-items: center; text-align: center; }
        .right-column { grid-column: 3 / 4; grid-row: 1 / 3; display: flex; flex-direction: column; gap: 10px; align-items: center; }
        .system-reset-area { grid-column: 1 / 4; grid-row: 3 / 4; display: flex; justify-content: center; gap: 10px; margin-top: 10px;}
        .status-display { grid-column: 1 / 4; grid-row: 2 / 3; padding: 15px; border-radius: 10px; background: #e9ecef; box-shadow: inset 0 2px 4px rgba(0,0,0,0.06); text-align: left; }

        h2, h3 { color: #222; margin-bottom: 15px; }
        button { padding:12px 25px; border:none; border-radius:6px; background:#007bff; color:white; font-size:16px; cursor:pointer; transition: background 0.3s ease, transform 0.2s ease, box-shadow 0.2s ease; box-shadow: 0 2px 4px rgba(0,0,0,0.1); width: 100%; max-width: 200px; }
        button:hover { background:#0056b3; transform: translateY(-1px); box-shadow: 0 4px 8px rgba(0,0,0,0.2); }
        .circular-button { width: 80px; height: 80px; border-radius: 50%; display: flex; align-items: center; justify-content: center; font-size: 1.2em; font-weight: bold; border: 2px solid #fff; flex-shrink: 0; }
        pre { background:#e9ecef; padding:15px; border-radius:8px; width: 100%; max-width:350px; margin:20px auto 0 auto; text-align:left; white-space: pre-wrap; word-wrap: break-word; font-size: 0.9em; line-height: 1.6; color: #444; border: 1px solid #dee2e6; overflow-y: auto; max-height: 250px; }
        select { padding:10px; border-radius:6px; border:1px solid #ccc; width: calc(100% - 22px); max-width: 178px; font-size:16px;}
        p { font-size: 0.9em; color: #555; margin-bottom: 10px; }
        .footer { margin-top: auto; padding-top: 20px; border-top: 1px solid #eee; width: 100%; }

        /* Custom Modal Styles */
        .modal {
            display: none; /* Hidden by default */
            position: fixed; /* Stay in place */
            z-index: 1000; /* Sit on top */
            left: 0;
            top: 0;
            width: 100%; /* Full width */
            height: 100%; /* Full height */
            overflow: auto; /* Enable scroll if needed */
            background-color: rgba(0,0,0,0.4); /* Black w/ opacity */
            justify-content: center;
            align-items: center;
        }
        .modal-content {
            background-color: #fefefe;
            margin: auto;
            padding: 20px;
            border: 1px solid #888;
            border-radius: 10px;
            width: 80%;
            max-width: 400px;
            box-shadow: 0 5px 15px rgba(0,0,0,0.3);
            text-align: center;
        }
        .modal-buttons {
            margin-top: 20px;
            display: flex;
            justify-content: space-around;
            gap: 10px;
        }
        .modal-buttons button {
            width: 120px;
        }
    </style>
</head>
<body>
    <h2>ESP32 Tube Pressure Tester</h2>

    <div class='main-layout'>

        <div class='left-column grid-area'>
            <h3>Tube Positioning</h3>
            <button onclick='moveStepperJs(10)'>Move 10cm</button>
            <button onclick='moveStepperJs(30)'>Move 30cm</button>
            <button onclick='moveStepperJs(60)'>Move 60cm</button>

            <select id='presetDistanceSelect'>
                <option value=''>Select distance</option>
                <option value='5'>5 cm</option>
                <option value='15'>15 cm</option>
                <option value='25'>25 cm</option>
                <option value='40'>40 cm</option>
                <option value='50'>50 cm</option>
                <option value='70'>70 cm</option>
                <option value='80'>80 cm</option>
                <option value='90'>90 cm</option>
            </select>
            <button onclick='moveStepperPresetJs()'>Move Selected cm</button>
        </div>

        <div class='center-data-display grid-area'>
            <h3>Flexural Test Graph</h3>
            <div class="relative h-64 md:h-80 w-full">
                <canvas id="flexuralChart"></canvas>
            </div>
            <h3 class="mt-4">Force Readings History</h3>
            <pre id='forceHistoryDisplay'></pre>
        </div>

        <div class='right-column grid-area'>
            <h3>Flexural Test Controls</h3>
            <button id="startTestBtn" class="bg-blue-600 hover:bg-blue-700 text-white font-bold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out transform hover:scale-105 focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-opacity-75">
                Start Test
            </button>
            <button id="stopTestBtn" class="bg-red-600 hover:bg-red-700 text-white font-bold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out transform hover:scale-105 focus:outline-none focus:ring-2 focus:ring-red-500 focus:ring-opacity-75" disabled>
                Stop Test
            </button>
            <button id="clearGraphBtn" class="bg-gray-400 hover:bg-gray-500 text-white font-bold py-3 px-6 rounded-lg shadow-md transition duration-300 ease-in-out transform hover:scale-105 focus:outline-none focus:ring-2 focus:ring-gray-300 focus:ring-opacity-75">
                Clear Graph
            </button>

            <h3 class="mt-6">Individual Arm & Force</h3>
            <button class='circular-button' style='background:#ffc107;color:#000;' onclick='resetForceData()'>Reset Force</button>
            <button style='background:#28a745;' onclick='measureCurrentForce()'>Measure current force</button>

            <select id='servoAngleSelect'>
                <option value=''>Select Angle</option>
                <option value='0'>0 deg (Retract)</option>
                <option value='5'>5 deg</option>
                <option value='10'>10 deg</option>
                <option value='15'>15 deg</option>
                <option value='20'>20 deg</option>
                <option value='25'>25 deg</option>
                <option value='30'>30 deg</option>
                <option value='35'>35 deg</option>
                <option value='40'>40 deg</option>
                <option value='45'>45 deg</option>
                <option value='50'>50 deg</option>
                <option value='55'>55 deg</option>
                <option value='60'>60 deg</option>
                <option value='90'>90 deg (Max Pressure)</option>
            </select>
            <button style='background:#6c757d;' onclick='moveServoPresetJs()'>Move Arm</button>
        </div>

        <div class='system-reset-area grid-area'>
            <p class="text-sm text-gray-600">
                <strong>Status:</strong> <span id="statusText" class="font-medium text-blue-600">Disconnected</span>
            </p>
            <p class="text-sm text-gray-600 mt-2">
                <strong>Last Data:</strong> <span id="lastDataText" class="font-mono text-gray-700">N/A</span>
            </p>
            <button style='background:#f44336;' onclick='showWifiResetModal()'>WIFI reset</button>
        </div>

    </div>

    <div class='footer'><p>Access URL: <strong><span id="accessUrl">Loading...</span></strong></p></div>

    <!-- Custom Confirmation Modal -->
    <div id="confirmationModal" class="modal">
        <div class="modal-content">
            <p>Are you sure you want to reset Wi-Fi settings and reboot the ESP32? This will disconnect you.</p>
            <div class="modal-buttons">
                <button onclick="confirmWifiReset(true)" style="background:#f44336;">Yes, Reset</button>
                <button onclick="confirmWifiReset(false)" style="background:#6c757d;">No, Cancel</button>
            </div>
        </div>
    </div>

    <script>
        // Use 'var' for function declarations to avoid C++ compiler issues with 'function' keyword
        var statusText = document.getElementById('statusText');
        var lastDataText = document.getElementById('lastDataText');
        var startTestBtn = document.getElementById('startTestBtn');
        var stopTestBtn = document.getElementById('stopTestBtn');
        var clearGraphBtn = document.getElementById('clearGraphBtn');
        var accessUrlSpan = document.getElementById('accessUrl');
        var confirmationModal = document.getElementById('confirmationModal');

        var socket;
        var chart;
        var chartData = {
            labels: [], // Displacement (cm)
            datasets: [{
                label: 'Force (N)',
                data: [], // Force (N)
                borderColor: 'rgb(75, 192, 192)',
                tension: 0.1,
                fill: false,
                pointRadius: 3,
                pointBackgroundColor: 'rgb(75, 192, 192)'
            }]
        };

        // Initialize Chart.js
        var ctx = document.getElementById('flexuralChart').getContext('2d');
        chart = new Chart(ctx, {
            type: 'line',
            data: chartData,
            options: {
                responsive: true,
                maintainAspectRatio: false,
                scales: {
                    x: {
                        type: 'linear', // Use linear scale for displacement
                        position: 'bottom',
                        title: {
                            display: true,
                            text: 'Displacement (cm)',
                            font: { size: 14, weight: 'bold' }
                        },
                        min: 0 // Start x-axis from 0
                    },
                    y: {
                        title: {
                            display: true,
                            text: 'Force (N)',
                            font: { size: 14, weight: 'bold' }
                        },
                        min: 0 // Start y-axis from 0
                    }
                },
                plugins: {
                    tooltip: {
                        callbacks: {
                            label: function(context) {
                                var label = context.dataset.label || '';
                                if (label) {
                                    label += ': ';
                                }
                                if (context.parsed.y !== null) {
                                    label += `${context.parsed.y.toFixed(3)} N at ${context.parsed.x.toFixed(3)} cm`;
                                }
                                return label;
                            }
                        }
                    }
                },
                animation: {
                    duration: 0 // Disable animation for real-time updates
                }
            }
        });

        var connectWebSocket = function() { // Changed to var functionName = function() {}
            var esp32Ip = window.location.hostname;
            accessUrlSpan.textContent = `http://${esp32Ip}/`; // Display current access URL
            socket = new WebSocket(`ws://${esp32Ip}/ws`);

            socket.onopen = function() {
                statusText.textContent = 'Connected';
                statusText.classList.remove('text-red-600', 'text-gray-600');
                statusText.classList.add('text-green-600');
                startTestBtn.disabled = false;
                stopTestBtn.disabled = true; // Initially disabled
                console.log('WebSocket connection opened');
            };

            socket.onmessage = function(event) {
                try {
                    var data = JSON.parse(event.data);
                    if (data.type === 'data') {
                        var displacement = parseFloat(data.displacement);
                        var force = parseFloat(data.force);

                        chartData.labels.push(displacement); // X-axis
                        chartData.datasets[0].data.push({ x: displacement, y: force }); // Y-axis
                        chart.update();

                        lastDataText.textContent = `Force: ${force.toFixed(3)} N, Disp: ${displacement.toFixed(3)} cm`;
                    } else if (data.type === 'status') {
                        statusText.textContent = data.message;
                        if (data.message === 'Test Running') {
                            startTestBtn.disabled = true;
                            stopTestBtn.disabled = false;
                            statusText.classList.remove('text-green-600', 'text-red-600');
                            statusText.classList.add('text-blue-600');
                        } else if (data.message === 'Test Finished' || data.message === 'Test Stopped') {
                            startTestBtn.disabled = false;
                            stopTestBtn.disabled = true;
                            statusText.classList.remove('text-blue-600');
                            statusText.classList.add('text-green-600');
                        } else if (data.message === 'Connected') {
                            startTestBtn.disabled = false;
                            stopTestBtn.disabled = true;
                            statusText.classList.remove('text-red-600', 'text-gray-600');
                            statusText.classList.add('text-green-600');
                        }
                    }
                } catch (e) {
                    console.error("Error parsing WebSocket message:", e, event.data);
                }
            };

            socket.onclose = function() {
                statusText.textContent = 'Disconnected';
                statusText.classList.remove('text-green-600', 'text-blue-600');
                statusText.classList.add('text-red-600');
                startTestBtn.disabled = true;
                stopTestBtn.disabled = true;
                console.log('WebSocket connection closed. Retrying in 3 seconds...');
                setTimeout(connectWebSocket, 3000); // Attempt to reconnect
            };

            socket.onerror = function(error) {
                console.error('WebSocket error:', error);
                statusText.textContent = 'Error';
                statusText.classList.remove('text-green-600', 'text-blue-600');
                statusText.classList.add('text-red-600');
            };
        }; // End of connectWebSocket function

        // Event Listeners for new Flexural Test buttons
        startTestBtn.addEventListener('click', function() {
            if (socket && socket.readyState === WebSocket.OPEN) {
                socket.send('START_TEST');
                startTestBtn.disabled = true;
                stopTestBtn.disabled = false;
                statusText.textContent = 'Starting Test...';
                statusText.classList.remove('text-green-600', 'text-red-600');
                statusText.classList.add('text-blue-600');
            } else {
                console.warn('WebSocket not connected. Cannot start test.');
                statusText.textContent = 'Not Connected';
                statusText.classList.remove('text-green-600', 'text-blue-600');
                statusText.classList.add('text-red-600');
            }
        });

        stopTestBtn.addEventListener('click', function() {
            if (socket && socket.readyState === WebSocket.OPEN) {
                socket.send('STOP_TEST');
                stopTestBtn.disabled = true;
                startTestBtn.disabled = false;
                statusText.textContent = 'Stopping Test...';
                statusText.classList.remove('text-green-600', 'text-red-600');
                statusText.classList.add('text-blue-600');
            } else {
                console.warn('WebSocket not connected. Cannot stop test.');
                statusText.textContent = 'Not Connected';
                statusText.classList.remove('text-green-600', 'text-blue-600');
                statusText.classList.add('text-red-600');
            }
        });

        clearGraphBtn.addEventListener('click', function() {
            chartData.labels = [];
            chartData.datasets[0].data = [];
            chart.update();
            lastDataText.textContent = 'N/A';
            console.log('Graph cleared.');
        });

        // --- Existing JavaScript functions (modified for alerts/confirms) ---

        // Function to handle fixed stepper move buttons via Fetch API (no page reload)
        var moveStepperJs = function(distance) { // Changed to var functionName = function() {}
            console.log('Attempting to move stepper ' + distance + 'cm');
            fetch('/move' + distance + 'cm_only')
                .then(function(response) {
                    if (!response.ok) {
                        console.error('Failed to move stepper ' + distance + 'cm');
                    }
                    console.log('Stepper move command sent for ' + distance + 'cm');
                })
                .catch(function(error) { console.error('Network error during stepper move:', error); });
        };

        // Function to handle preset stepper move button via Fetch API
        var moveStepperPresetJs = function() { // Changed to var functionName = function() {}
            var selectElement = document.getElementById('presetDistanceSelect');
            var distance = parseFloat(selectElement.value);
            if (isNaN(distance) || distance <= 0) {
                console.error('Invalid input: Please select a valid distance from the dropdown.');
                // Optionally, update a status message on the page here
                return;
            }
            console.log('Attempting to move stepper ' + distance + 'cm (preset)');
            fetch('/move_custom_cm_only?distance=' + distance)
                .then(function(response) {
                    if (!response.ok) {
                        console.error('Failed to move stepper ' + distance + 'cm (preset)');
                    }
                    console.log('Preset stepper move command sent for ' + distance + 'cm');
                    selectElement.value = ''; // Reset dropdown after sending
                })
                .catch(function(error) { console.error('Network error during preset stepper move:', error); });
        };

        // Function to fetch and update force history
        var updateForceHistory = function() { // Changed to var functionName = function() {}
            fetch('/get_history')
                .then(function(response) { return response.text(); })
                .then(function(data) {
                    document.getElementById('forceHistoryDisplay').innerText = data;
                })
                .catch(function(error) { console.error('Error fetching force history:', error); });
        };

        // Function to handle Reset Force data via Fetch API
        var resetForceData = function() { // Changed to var functionName = function() {}
            console.log('Resetting force data...');
            fetch('/reset_force_data')
                .then(function(response) {
                    if (response.ok) {
                        console.log('Force data reset command sent.');
                        updateForceHistory(); // Immediately clear history display
                    } else {
                        console.error('Failed to reset force data.');
                    }
                })
                .catch(function(error) { console.error('Network error during force reset:', error); });
        };

        // Function to handle Measure current force via Fetch API
        var measureCurrentForce = function() { // Changed to var functionName = function() {}
            console.log('Measuring current force...');
            fetch('/read_force')
                .then(function(response) {
                    if (response.ok) {
                        console.log('Measure force command sent.');
                    } else {
                        console.error('Failed to measure force.');
                    }
                })
                .catch(function(error) { console.error('Network error during force measurement:', error); });
        };

        // Function to handle Preset Servo moves via Fetch API (uses new generic endpoint)
        var moveServoPresetJs = function() { // Changed to var functionName = function() {}
            var selectElement = document.getElementById('servoAngleSelect');
            var angle = parseInt(selectElement.value);
            if (isNaN(angle) || angle < 0 || angle > 180) { // Basic validation for servo angle
                console.error('Invalid input: Please select a valid angle from the dropdown (0-180 degrees).');
                // Optionally, update a status message on the page here
                return;
            }
            console.log('Attempting to move servo to ' + angle + ' degrees (preset)');
            fetch('/move_servo?angle=' + angle) // NEW generic endpoint
                .then(function(response) {
                    if (!response.ok) {
                        console.error('Failed to move servo to ' + angle + ' degrees.');
                    }
                    console.log('Servo move command sent for ' + angle + ' degrees.');
                    selectElement.value = ''; // Reset dropdown after sending
                })
                .catch(function(error) { console.error('Network error during servo move:', error); });
        };
        
        // Custom modal functions for Wi-Fi reset
        var showWifiResetModal = function() { // Changed to var functionName = function() {}
            confirmationModal.style.display = 'flex'; // Show the modal
        };

        var confirmWifiReset = function(confirmed) { // Changed to var functionName = function() {}
            confirmationModal.style.display = 'none'; // Hide the modal
            if (confirmed) {
                console.log('Initiating Wi-Fi reset and reboot...');
                fetch('/reset')
                    .then(function(response) {
                        console.log('Wi-Fi reset command sent. ESP32 should reboot shortly.');
                    })
                    .catch(function(error) { console.error('Network error during Wi-Fi reset (expected if rebooting):', error); });
            } else {
                console.log('Wi-Fi reset cancelled.');
            }
        };

        // Update history initially and then every 2 seconds
        document.addEventListener('DOMContentLoaded', function() {
            updateForceHistory();
            setInterval(updateForceHistory, 2000); // Update every 2 seconds
            connectWebSocket(); // Initial WebSocket connection attempt
        });
    </script>
</body>
</html>
)rawliteral";


void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ESP32 Load Cell & Stepper Setup ---");

  // Load cell setup (using HX711.h)
  scale.begin(DT, SCK); // Using begin(DT, SCK) from HX711.h
  scale.set_scale(calibration_factor); // Using set_scale() from HX711.h
  scale.tare(); // Tare the scale at startup
  Serial.println("HX711 detected and ready."); // Assuming it's ready after begin() and tare()
  Serial.println("HX711 calibration set and tared. Perform actual calibration if needed!");

  // Stepper Motor Setup
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(ENABLE_PIN, OUTPUT);
  digitalWrite(ENABLE_PIN, LOW); // Disable motor initially
  Serial.println("Stepper Motor Initialized.");

  // Servo Setup
  myServo.attach(SERVO_PIN);
  myServo.write(SERVO_RETRACT_ANGLE); // Ensure servo is at the retracted position
  Serial.println("Servo Motor Initialized.");

  // Reset button
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  WiFiManager wm;
  wm.setConfigPortalTimeout(40);
  bool connectedToRouter = wm.autoConnect("ESP32-Setup", "12345678");

  if (connectedToRouter) {
    Serial.println("✅ Connected to Wi-Fi!");
    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("esp32-FTM")) {
      Serial.println("🌐 mDNS responder started at http://esp32-FTM.local");
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

  // --- Web Server Endpoints ---

  // Serve the main page from PROGMEM
  server.on("/", []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  // Endpoint to get the force history dynamically
  server.on("/get_history", []() {
    server.send(200, "text/plain", forceHistory);
  });

  // --- Stepper Motor Only Endpoints ---
  server.on("/move10cm_only", []() {
    Serial.println("Web request: Moving 10cm (no measurement)...");
    moveTubeOnly(10.0);
    server.send(200, "text/plain", "OK");
  });

  server.on("/move30cm_only", []() {
    Serial.println("Web request: Moving 30cm (no measurement)...");
    moveTubeOnly(30.0);
    server.send(200, "text/plain", "OK");
  });

  server.on("/move60cm_only", []() {
    Serial.println("Web request: Moving 60cm (no measurement)...");
    moveTubeOnly(60.0);
    server.send(200, "text/plain", "OK");
  });

  // --- Endpoint for Custom/Preset Stepper Move (used by dropdown) ---
  server.on("/move_custom_cm_only", HTTP_GET, []() {
    if (server.hasArg("distance")) {
      float distance = server.arg("distance").toFloat();
      Serial.print("Web request: Moving ");
      Serial.print(distance);
      Serial.println("cm (custom/preset, no measurement)...");
      moveTubeOnly(distance);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing distance parameter");
    }
  });


  // --- Force Measurement Endpoint ---
  server.on("/read_force", []() {
    Serial.println("Web request: Measuring current force only...");
    sendCurrentForce();
    server.send(200, "text/plain", "OK"); // Send OK, JS will fetch new history
  });

  // Generic Servo Control Endpoint (handles all angles, including 0 for reset)
  server.on("/move_servo", HTTP_GET, []() {
    if (server.hasArg("angle")) {
      int angle = server.arg("angle").toInt();
      // Basic validation to keep servo within safe range (0-180 degrees)
      if (angle >= 0 && angle <= 180) {
        Serial.print("Web request: Moving servo to ");
        Serial.print(angle);
        Serial.println(" degrees...");
        moveServo(angle);
        server.send(200, "text/plain", "OK");
      } else {
        server.send(400, "text/plain", "Invalid angle. Angle must be between 0 and 180.");
      }
    } else {
      server.send(400, "text/plain", "Missing angle parameter");
    }
  });

  // --- System Control Endpoints ---
  server.on("/reset_force_data", []() {
    forceHistory = "";
    readingCounter = 0;
    Serial.println("Force data reset.");
    server.send(200, "text/plain", "OK");
  });

  server.on("/reset", []() {
    server.send(200, "text/html", "<h3>Resetting Wi-Fi and rebooting...</h3>");
    delay(1000);
    WiFi.disconnect(true, true);
    ESP.restart();
  });

  server.begin();
  Serial.println("📡 Web server started.");

  // WebSocket server setup
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81");
}

void loop() {
  server.handleClient();
  webSocket.loop(); // Important: Keep WebSocket server running
  // HX711.h does not require a continuous update() call

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

  // --- Flexural Test Logic (triggered by webSocketEvent) ---
  if (testRunning) {
      if (currentServoAngle <= SERVO_TEST_END_ANGLE) {
          myServo.write(currentServoAngle); // Move servo
          delay(50); // Small delay for servo to move and stabilize

          if (millis() - lastDataSendTime > DATA_SEND_INTERVAL) {
              if (scale.is_ready()) { // Using is_ready() from HX711.h
                  float currentForce = scale.get_units(10) * 0.00980665; // Using get_units(10) from HX711.h
                  // Calculate displacement based on servo angle and calibration
                  float currentDisplacement = (currentServoAngle - SERVO_RETRACT_ANGLE) * (1.3 / 60.0); // 1.3cm for 60 degrees

                  // Create JSON string to send over WebSocket
                  String jsonString = "{\"type\":\"data\",\"displacement\":";
                  jsonString += String(currentDisplacement, 3); // 3 decimal places
                  jsonString += ",\"force\":";
                  jsonString += String(currentForce, 3); // 3 decimal places
                  jsonString += "}";

                  webSocket.broadcastTXT(jsonString); // Send data to all connected clients
                  Serial.println(jsonString); // Print to serial for debugging

                  lastDataSendTime = millis();
                  currentServoAngle += SERVO_TEST_STEP_ANGLE; // Increment angle for next step
              } else {
                  Serial.println("HX711 not ready for continuous data during test.");
              }
          }
      } else {
          // Test finished
          testRunning = false;
          webSocket.broadcastTXT("{\"type\":\"status\",\"message\":\"Test Finished\"}");
          Serial.println("Flexural test finished.");
          myServo.write(SERVO_RETRACT_ANGLE); // Retract servo after test
      }
  }
}
