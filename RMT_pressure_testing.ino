#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "hadi";
const char* password = "12345678";

WebServer server(80);

// Example sensor data
float getSensorData() {
  return random(200, 300) / 10.0;
}

// HTML page with Chart.js
const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Graph</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<body>
  <h2>Live Sensor Graph</h2>
  <canvas id="myChart" width="400" height="200"></canvas>
  <script>
    const ctx = document.getElementById('myChart').getContext('2d');
    const labels = [];
    const data = {
      labels: labels,
      datasets: [{
        label: 'Sensor Data',
        borderColor: 'rgb(75, 192, 192)',
        data: [],
        fill: false,
      }]
    };
    const config = {
      type: 'line',
      data: data,
      options: {
        responsive: true,
        animation: false,
        scales: {
          x: { title: { display: true, text: 'Time' } },
          y: { title: { display: true, text: 'Value' } }
        }
      }
    };
    const myChart = new Chart(ctx, config);

    function fetchData() {
      fetch('/data')
        .then(response => response.text())
        .then(value => {
          const now = new Date().toLocaleTimeString();
          if (labels.length > 20) {
            labels.shift();
            data.datasets[0].data.shift();
          }
          labels.push(now);
          data.datasets[0].data.push(parseFloat(value));
          myChart.update();
        });
    }

    setInterval(fetchData, 1000);  // Fetch every second
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", html);
}

void handleData() {
  float sensorValue = getSensorData();
  server.send(200, "text/plain", String(sensorValue));
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected");

  server.on("/", handleRoot);
  server.on("/data", handleData);

  server.begin();
}

void loop() {
  server.handleClient();
}
