#include <ESP32Servo.h>  // Make sure this library is installed (NOT the regular Servo library)

Servo myServo;           // Create servo object

const int SERVO_PIN = 13;  // You can use pins like 13, 14, 15, 2, 4, 16, 17, 18, or 19

void setup() {
  Serial.begin(115200);         // Initialize serial communication
  myServo.setPeriodHertz(50);   // Standard 50Hz servo
  myServo.attach(SERVO_PIN);    // Attach the servo to the selected pin
  Serial.println("ESP32 Servo Test");
}

void loop() {
  myServo.write(0);             // Move to 0°
  Serial.println("Angle: 0");
  delay(2000);

  myServo.write(180);           // Move to 180°
  Serial.println("Angle: 180");
  delay(2000);
}
