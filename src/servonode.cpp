/*
 * SFGE Servo Node — Waveshare ESP32-S3-Zero driving a head servo
 * (via the ESP32Servo library — proven on this hardware family)
 *
 * Destiny: a mesh peripheral the droid commands over ESP-NOW (same pattern
 * as the audio node). FOR NOW: a standalone servo exerciser —
 *
 *   Servo signal on GPIO 5 (servo powered from 5V, grounds common).
 *   Every 5 seconds the head turns 30 degrees to one side, then the other
 *   (90 -> 120 -> 60 -> 120 -> ...), easing smoothly between positions.
 *
 * Serial (native USB) logs every move at 115200.
 */
#include <Arduino.h>
#include <ESP32Servo.h>
#define S Serial

#define SERVO_PIN   5
#define SERVO_MIN_US 600
#define SERVO_MAX_US 2400
#define CENTER_DEG  90
#define SWING_DEG   30
#define PERIOD_MS   5000

Servo head;
int headDeg = CENTER_DEG;
int headTarget = CENTER_DEG;

void setup() {
    S.begin(115200);
    delay(400);
    S.println("\n================================");
    S.println("  SFGE SERVO NODE — exerciser");
    S.println("================================");

    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    head.setPeriodHertz(50);
    head.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
    head.write(headDeg);
    S.printf("[SERVO] ESP32Servo on GPIO %d — sweeping +/-%d deg every %d s\n",
             SERVO_PIN, SWING_DEG, PERIOD_MS / 1000);
}

void loop() {
    // Every PERIOD_MS, swing to the other side of center
    static unsigned long nextMove = PERIOD_MS;
    static bool rightSide = true;
    if (millis() > nextMove) {
        nextMove = millis() + PERIOD_MS;
        headTarget = CENTER_DEG + (rightSide ? SWING_DEG : -SWING_DEG);
        rightSide = !rightSide;
        S.printf("[SERVO] target %d deg\n", headTarget);
    }

    // Step one degree at a time toward the target (smooth, ~15 ms/deg —
    // the same pacing as the proven radar-dish sketch)
    static unsigned long lastStep = 0;
    if (headDeg != headTarget && millis() - lastStep >= 15) {
        lastStep = millis();
        headDeg += (headTarget > headDeg) ? 1 : -1;
        head.write(headDeg);
    }

    delay(2);
}
