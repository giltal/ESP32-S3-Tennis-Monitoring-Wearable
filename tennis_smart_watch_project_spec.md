# 🎾 Wearable Tennis Performance Analysis System (Version 0.1)

## 📌 Introduction
A wearable system based on ESP32-S3, designed as a smart watch worn on the player's hitting hand. The system measures and analyzes tennis stroke quality in real time using an accelerometer and gyroscope.

---

## 🎯 Objectives
- Real-time hit detection
- Measure power, stability, and spin
- Detect rally sequences
- Collect user feedback on stroke quality
- Enable personalized learning and improvement
- Final goal - Improve tennis player's overall game quality by providing real time data of striking quality, successes statistics and more.

---

## 🧠 System Overview

### Components:
- ESP32-S3
- IMU (Accelerometer + Gyroscope)
- Touch display
- BLE \ WiFi (optional)
- SD card

---

## 🏗️ Architecture

```
IMU → Filtering → Sensor Fusion → Hit Detection → Feature Extraction → Scoring → UI / BLE
```

---

## ⚙️ Algorithms

### Hit Detection
- Threshold-based detection on accelerometer and gyroscope
- Short time window (~30ms)

### Power Calculation
- Peak acceleration and angular velocity

### Spin Detection
- Ratio between gyroscope and acceleration

### Rally Detection
- Based on time between hits

---

## 🧪 Feature Extraction

For each hit:
- max_accel
- max_gyro
- spin_score
- duration
- stability

---

## 🎮 User Interface

### Real-Time
- Haptic feedback based on hit quality

### Post-Hit Classification
User selects:
- Good hit (point won)
- Good hit but out
- Bad hit
- Winner

---

## ⚠️ Challenges
- Sensor noise
- Wrist vs racket motion differences
- Accurate impact detection
- Latency constraints

---

## 🛠️ Development Plan

### Phase 1: Basics
- Read IMU data
- Display raw data

### Phase 2: Hit Detection
- Implement threshold-based detection

### Phase 3: Power Estimation
- Detect peak values

### Phase 4: Spin Detection
- Compute gyro/accel ratio

### Phase 5: UI
- Implement user input

### Phase 6: Rally Detection
- Identify hit sequences

### Phase 7: Scoring
- Assign score per hit

### Phase 8: Learning
- Personalization based on user feedback

---

## 🚀 Advanced Stage
- TinyML integration
- Stroke type classification
- Full match analysis

---

## 🔮 Future Extensions
- Mobile application
- Player comparison
- Integration with video analysis

