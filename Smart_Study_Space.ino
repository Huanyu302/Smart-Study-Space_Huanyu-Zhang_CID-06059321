#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include "MAX30105.h"

// ========== Hardware Pins ==========
#define SOUND_PIN     34
#define LED_PIN       13
#define LED_COUNT     12
#define I2C_SDA       4
#define I2C_SCL       15

// ========== Sound Sensor Configuration ==========
#define SOUND_P2P_MIN    500.0
#define SOUND_P2P_MAX    1500.0
#define SOUND_DB_MIN     25.0
#define SOUND_DB_MAX     85.0
#define SOUND_FILTER_SIZE   5

unsigned int soundSignalMax = 0;
unsigned int soundSignalMin = 4095;
unsigned int soundPeakToPeak = 0;
float soundDB = 30.0;
float soundBuffer[SOUND_FILTER_SIZE];
int soundIndex = 0;

// ========== Heart Rate Sensor - Ultra Sensitive Version ==========
#define RATE_SIZE     4
#define IR_THRESHOLD  50000

// Heart rate detection variables
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
long irValue = 0;
int beatsPerMinute = 0;              // Real-time BPM
int beatAvg = 0;                     // Average BPM
bool fingerDetected = false;

// Enhanced heart beat detection status
bool beatDetected = false;
int beatCount = 0;
unsigned long firstBeatTime = 0;

// IR change monitoring - using floating window
#define IR_WINDOW_SIZE 30
long irWindow[IR_WINDOW_SIZE];
int irWindowIndex = 0;
bool irWindowFull = false;

// Adaptive threshold variables
long irBaseline = 0;
long irAmplitude = 0;
int stableReadings = 0;

// AC/DC component analysis
long irDC = 0;
long irAC = 0;
long irMax = 0;
long irMin = 999999;

// Heart Rate Variability (HRV)
#define RR_INTERVAL_SIZE  15
unsigned long rrIntervals[RR_INTERVAL_SIZE];
int rrIndex = 0;
float hrv_SDNN = 0.0;
float hrv_RMSSD = 0.0;
int hrvSampleCount = 0;

// ========== WiFi and ThingSpeak ==========
const char* ssid = "teacup";
const char* password = "74375625";
String apiKey = "QI4MC7AC572BDE60";
const char* thingSpeakServer = "api.thingspeak.com";
const unsigned long uploadInterval = 8000;
unsigned long lastUploadTime = 0;

// ========== Global Objects ==========
MAX30105 heartSensor;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WebServer server(80);

// ========== System Variables ==========
String systemStatus = "READY";
bool max30102OK = false;
unsigned long recordCount = 0;

unsigned long lastSensorRead = 0;
unsigned long lastOLEDUpdate = 0;
unsigned long lastLEDUpdate = 0;
unsigned long lastDebugPrint = 0;

const unsigned long sensorInterval = 100;     // 100ms - reduced sampling rate for better signal stability
const unsigned long oledInterval = 500;
const unsigned long ledInterval = 100;
const unsigned long debugInterval = 5000;     // 5 seconds - less frequent debug output for clarity

// ========== Data Logging ==========
struct StudySession {
  unsigned long timestamp;
  float noiseLevel;
  int heartRate;
  float hrv;
  String status;
};

#define MAX_LOG_ENTRIES 50
StudySession sessionLog[MAX_LOG_ENTRIES];
int logIndex = 0;
bool logFull = false;

// ========== Function Declarations ==========
void initHardware();
void initWiFi();
void initWebServer();
void readSoundSensor();
void readHeartSensor_UltraSensitive();
void detectHeartBeat_Adaptive();
void calculateHRV();
void updateIRWindow();
void calculateACDC();
void logSessionData();
void updateSystemStatus();
void updateOLED();
void updateLED();
void uploadToThingSpeak();
void printDetailedDebug();
void handleRoot();
void handleData();
void handleNotFound();

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n========================================");
  Serial.println("  Smart Study Space");
  Serial.println("  Adaptive Heart Rate Detection");
  Serial.println("  Detailed Debug Mode");
  Serial.println("========================================\n");
  
  initHardware();
  initWiFi();
  initWebServer();
  
  Serial.println("\n========================================");
  Serial.println("System Ready!");
  Serial.println("Place finger firmly on sensor");
  Serial.println("Keep completely still for 15 seconds");
  Serial.println("========================================\n");
  
  // Initialize buffers
  for(int i = 0; i < SOUND_FILTER_SIZE; i++) {
    soundBuffer[i] = 30.0;
  }
  
  for(int i = 0; i < RR_INTERVAL_SIZE; i++) {
    rrIntervals[i] = 0;
  }
  
  for(int i = 0; i < IR_WINDOW_SIZE; i++) {
    irWindow[i] = 0;
  }
}

// ========== Main Loop ==========
void loop() {
  unsigned long currentMillis = millis();
  
  server.handleClient();
  
  if (currentMillis - lastSensorRead >= sensorInterval) {
    lastSensorRead = currentMillis;
    readSoundSensor();
    readHeartSensor_UltraSensitive();
    updateSystemStatus();
  }
  
  if (currentMillis - lastOLEDUpdate >= oledInterval) {
    lastOLEDUpdate = currentMillis;
    updateOLED();
  }
  
  if (currentMillis - lastLEDUpdate >= ledInterval) {
    lastLEDUpdate = currentMillis;
    updateLED();
  }
  
  if (currentMillis - lastDebugPrint >= debugInterval) {
    lastDebugPrint = currentMillis;
    printDetailedDebug();
  }
  
  if (currentMillis - lastUploadTime >= uploadInterval) {
    lastUploadTime = currentMillis;
    logSessionData();
    uploadToThingSpeak();
    recordCount++;
  }
}

// ========== Hardware Initialization ==========
void initHardware() {
  Serial.println("Initializing hardware...");
  
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("I2C: SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);
  
  Serial.print("[1/3] OLED...");
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(" FAILED!");
  } else {
    Serial.println(" OK");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println("Initializing...");
    display.display();
  }
  
  Serial.print("[2/3] MAX30102...");
  if (!heartSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println(" FAILED!");
    max30102OK = false;
  } else {
    Serial.println(" OK");
    
    byte ledBrightness = 0x1F;
    byte sampleAverage = 4;
    byte ledMode = 2;
    int sampleRate = 100;
    int pulseWidth = 411;
    int adcRange = 4096;
    
    heartSensor.setup(ledBrightness, sampleAverage, ledMode, 
                      sampleRate, pulseWidth, adcRange);
    
    max30102OK = true;
    Serial.println("        Adaptive algorithm loaded");
  }
  
  Serial.print("[3/3] LED Strip...");
  strip.begin();
  strip.setBrightness(50);
  strip.show();
  Serial.println(" OK");
  
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  
  Serial.println("\nHardware initialization complete!\n");
}

// ========== WiFi Initialization ==========
void initWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed");
  }
  Serial.println();
}

// ========== Web Server Initialization ==========
void initWebServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();
  
  Serial.println("Web server started");
  Serial.print("URL: http://");
  Serial.println(WiFi.localIP());
  Serial.println();
}

// ========== Sound Sensor Reading ==========
void readSoundSensor() {
  unsigned long startMillis = millis();
  soundSignalMax = 0;
  soundSignalMin = 4095;
  
  while (millis() - startMillis < 50) {
    int sample = analogRead(SOUND_PIN);
    if (sample > soundSignalMax) soundSignalMax = sample;
    if (sample < soundSignalMin) soundSignalMin = sample;
  }
  
  soundPeakToPeak = soundSignalMax - soundSignalMin;
  
  if (soundPeakToPeak < SOUND_P2P_MIN) {
    soundDB = SOUND_DB_MIN;
  } else if (soundPeakToPeak > SOUND_P2P_MAX) {
    soundDB = SOUND_DB_MAX;
  } else {
    float ratio = (float)(soundPeakToPeak - SOUND_P2P_MIN) / (SOUND_P2P_MAX - SOUND_P2P_MIN);
    soundDB = SOUND_DB_MIN + ratio * (SOUND_DB_MAX - SOUND_DB_MIN);
  }
  
  soundBuffer[soundIndex] = soundDB;
  soundIndex = (soundIndex + 1) % SOUND_FILTER_SIZE;
  
  float sum = 0;
  for(int i = 0; i < SOUND_FILTER_SIZE; i++) {
    sum += soundBuffer[i];
  }
  soundDB = sum / SOUND_FILTER_SIZE;
}

// ========== Ultra Sensitive Heart Sensor Reading ==========
void readHeartSensor_UltraSensitive() {
  if (!max30102OK) {
    irValue = 0;
    fingerDetected = false;
    beatsPerMinute = 0;
    beatAvg = 0;
    return;
  }
  
  // Read IR value
  irValue = heartSensor.getIR();
  
  // Finger detection
  fingerDetected = (irValue > IR_THRESHOLD);
  
  if (!fingerDetected) {
    // Reset all variables when no finger
    beatsPerMinute = 0;
    beatAvg = 0;
    beatCount = 0;
    irBaseline = 0;
    stableReadings = 0;
    irWindowFull = false;
    irWindowIndex = 0;
    return;
  }
  
  // Add IR value to sliding window
  irWindow[irWindowIndex] = irValue;
  irWindowIndex++;
  if (irWindowIndex >= IR_WINDOW_SIZE) {
    irWindowIndex = 0;
    irWindowFull = true;
  }
  
  // Calculate AC/DC components and detect heartbeat
  if (irWindowFull) {
    calculateACDC();
    detectHeartBeat_Adaptive();
  }
}

// ========== Calculate AC/DC Components ==========
void calculateACDC() {
  // Calculate DC component (baseline - average)
  long sum = 0;
  irMax = irWindow[0];
  irMin = irWindow[0];
  
  for (int i = 0; i < IR_WINDOW_SIZE; i++) {
    sum += irWindow[i];
    if (irWindow[i] > irMax) irMax = irWindow[i];
    if (irWindow[i] < irMin) irMin = irWindow[i];
  }
  
  irBaseline = sum / IR_WINDOW_SIZE;
  irDC = irBaseline;
  
  // Calculate AC component (variation amplitude)
  irAmplitude = irMax - irMin;
  irAC = irAmplitude / 2;  // Approximate AC as half of peak-to-peak
}

// ========== Adaptive Heart Beat Detection ==========
void detectHeartBeat_Adaptive() {
  static long lastValue = 0;
  static bool inPulse = false;
  static unsigned long pulseStartTime = 0;
  static long peakValue = 0;
  
  // If AC component too small, signal is too weak
  if (irAC < 30) {
    // Signal too weak, need to adjust finger
    stableReadings = 0;
    return;
  }
  
  // Adaptive threshold: baseline + 30% of AC component
  long threshold = irBaseline + (irAC * 3 / 10);
  
  // Detect rising edge (heart beat start)
  if (!inPulse && irValue > threshold && lastValue <= threshold) {
    inPulse = true;
    pulseStartTime = millis();
    peakValue = irValue;
    stableReadings++;
  }
  
  // During pulse, track peak value
  if (inPulse) {
    if (irValue > peakValue) {
      peakValue = irValue;
    }
    
    // Detect falling edge (heart beat end)
    if (irValue < threshold) {
      inPulse = false;
      
      // Calculate BPM
      unsigned long currentTime = millis();
      unsigned long delta = currentTime - lastBeat;
      
      if (lastBeat > 0 && delta > 400 && delta < 3000) {
        beatsPerMinute = 60000 / delta;
        beatCount++;
        
        if (beatsPerMinute >= 45 && beatsPerMinute <= 150) {
          rates[rateSpot] = (byte)beatsPerMinute;
          
          // Store R-R interval for HRV
          rrIntervals[rrIndex] = delta;
          rrIndex = (rrIndex + 1) % RR_INTERVAL_SIZE;
          if (rrIndex == 0) hrvSampleCount = RR_INTERVAL_SIZE;
          else if (hrvSampleCount < RR_INTERVAL_SIZE) hrvSampleCount++;
          
          rateSpot = (rateSpot + 1) % RATE_SIZE;
          
          // Calculate average
          beatAvg = 0;
          int validCount = 0;
          for (byte x = 0; x < RATE_SIZE; x++) {
            if (rates[x] > 0) {
              beatAvg += rates[x];
              validCount++;
            }
          }
          if (validCount > 0) {
            beatAvg /= validCount;
          }
          
          calculateHRV();
          beatDetected = true;
        }
      }
      
      lastBeat = currentTime;
    }
  }
  
  lastValue = irValue;
}

// ========== HRV Calculation ==========
void calculateHRV() {
  if (hrvSampleCount < 5) {  // Need at least 5 R-R intervals for reliable HRV
    hrv_SDNN = 0.0;
    hrv_RMSSD = 0.0;
    return;
  }
  
  float mean = 0;
  for (int i = 0; i < hrvSampleCount; i++) {
    mean += rrIntervals[i];
  }
  mean /= hrvSampleCount;
  
  float variance = 0;
  for (int i = 0; i < hrvSampleCount; i++) {
    float diff = rrIntervals[i] - mean;
    variance += diff * diff;
  }
  variance /= hrvSampleCount;
  hrv_SDNN = sqrt(variance);
  
  float sumSquaredDiff = 0;
  int diffCount = 0;
  for (int i = 1; i < hrvSampleCount; i++) {
    long diff = rrIntervals[i] - rrIntervals[i-1];
    sumSquaredDiff += diff * diff;
    diffCount++;
  }
  if (diffCount > 0) {
    hrv_RMSSD = sqrt(sumSquaredDiff / diffCount);
  } else {
    hrv_RMSSD = 0;
  }
}

// ========== Log Session Data ==========
void logSessionData() {
  sessionLog[logIndex].timestamp = millis() / 1000;
  sessionLog[logIndex].noiseLevel = soundDB;
  sessionLog[logIndex].heartRate = beatAvg;
  sessionLog[logIndex].hrv = hrv_RMSSD;
  sessionLog[logIndex].status = systemStatus;
  
  logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;
  if (logIndex == 0) logFull = true;
}

// ========== System Status Update ==========
void updateSystemStatus() {
  if (WiFi.status() != WL_CONNECTED) {
    systemStatus = "OFFLINE";
  } else if (soundDB > 70) {
    systemStatus = "NOISY";
  } else if (soundDB > 60) {
    systemStatus = "ALERT";
  } else if (fingerDetected && beatAvg > 0) {
    if (hrv_RMSSD > 50) {
      systemStatus = "RELAXED";
    } else if (hrv_RMSSD < 20) {
      systemStatus = "STRESSED";
    } else if (beatAvg < 60) {
      systemStatus = "CALM";
    } else if (beatAvg > 90) {
      systemStatus = "ACTIVE";
    } else {
      systemStatus = "OPTIMAL";
    }
  } else {
    systemStatus = "READY";
  }
}

// ========== OLED Display ==========
void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  
  display.setCursor(0, 0);
  display.println("Smart Study Space");
  display.drawLine(0, 10, 128, 10, WHITE);
  
  display.setCursor(0, 13);
  display.printf("Noise: %.1f dB", soundDB);
  
  display.setCursor(0, 23);
  if (max30102OK && fingerDetected) {
    if (beatAvg > 0) {
      display.printf("HR: %d bpm", beatsPerMinute);
      display.setCursor(0, 33);
      display.printf("Avg: %d bpm", beatAvg);
      display.setCursor(0, 43);
      display.printf("HRV: %.1f ms", hrv_RMSSD);
    } else {
      display.printf("Detecting... %d", beatCount);
      display.setCursor(0, 33);
      if (irAC < 20) {
        display.printf("Signal: WEAK");
      } else if (stableReadings > 5) {
        display.printf("Signal: GOOD");
      } else {
        display.printf("Signal: OK");
      }
    }
  } else {
    display.print("HR: ---");
  }
  
  display.setCursor(0, 53);
  display.printf("Status: %s", systemStatus.c_str());
  
  display.setCursor(100, 53);
  display.printf("#%lu", recordCount);
  
  display.display();
}

// ========== LED Display ==========
void updateLED() {
  uint32_t color;
  
  if (soundDB < 40) {
    color = strip.Color(0, 255, 0);
  } else if (soundDB < 55) {
    color = strip.Color(0, 255, 255);
  } else if (soundDB < 70) {
    color = strip.Color(255, 255, 0);
  } else {
    color = strip.Color(255, 0, 0);
  }
  
  if (fingerDetected && beatAvg > 0) {
    if (hrv_RMSSD > 50) {
      color = strip.Color(0, 255, 128);
    } else if (hrv_RMSSD < 20) {
      color = strip.Color(255, 128, 0);
    }
  }
  
  if (beatDetected && (millis() % 1000 < 100)) {
    color = strip.Color(255, 0, 255);
  }
  beatDetected = false;
  
  for(int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

// ========== Detailed Serial Debug Output ==========
void printDetailedDebug() {
  Serial.println("\n======================================== DEBUG ========================================");
  Serial.printf("Time: %lu seconds | Uptime: %lu ms | Record: #%lu\n", 
                millis()/1000, millis(), recordCount);
  Serial.println("--------------------------------------------------------------------------------------");
  
  // === SOUND SENSOR ===
  Serial.println("\n[SOUND SENSOR]");
  Serial.printf("  Peak-to-Peak:  %d ADC\n", soundPeakToPeak);
  Serial.printf("  ADC Min:       %d\n", soundSignalMin);
  Serial.printf("  ADC Max:       %d\n", soundSignalMax);
  Serial.printf("  Noise Level:   %.1f dB\n", soundDB);
  
  if (soundPeakToPeak > 3000) {
    Serial.println("  WARNING: Signal saturated! Reduce gain.");
  } else if (soundPeakToPeak < 500) {
    Serial.println("  WARNING: Signal too weak! Increase gain.");
  } else {
    Serial.println("  Status: OK");
  }
  
  // === HEART RATE SENSOR ===
  Serial.println("\n[HEART RATE SENSOR]");
  Serial.printf("  Finger Detected:   %s\n", fingerDetected ? "YES" : "NO");
  
  if (fingerDetected) {
    Serial.printf("  IR Value:          %ld\n", irValue);
    Serial.printf("  IR Baseline (DC):  %ld\n", irBaseline);
    Serial.printf("  IR Amplitude:      %ld (Max: %ld, Min: %ld)\n", irAmplitude, irMax, irMin);
    Serial.printf("  IR AC Component:   %ld\n", irAC);
    Serial.printf("  Window Full:       %s\n", irWindowFull ? "YES" : "NO");
    Serial.printf("  Stable Readings:   %d\n", stableReadings);
    
    // Threshold calculation
    long threshold = irBaseline + (irAC * 3 / 10);
    Serial.printf("  Adaptive Threshold: %ld (Baseline + 30%% AC)\n", threshold);
    
    // Signal quality assessment
    Serial.print("  Signal Quality:    ");
    if (irAC < 30) {
      Serial.println("WEAK - Adjust finger position/pressure");
    } else if (irAC < 100) {
      Serial.println("FAIR - Keep finger still");
    } else if (irAC < 500) {
      Serial.println("GOOD");
    } else {
      Serial.println("EXCELLENT");
    }
    
    Serial.println("\n[HEART RATE DETECTION]");
    Serial.printf("  Real-time BPM:     %d bpm\n", beatsPerMinute);
    Serial.printf("  Average BPM:       %d bpm (last %d beats)\n", beatAvg, RATE_SIZE);
    Serial.printf("  Total Beats:       %d\n", beatCount);
    
    if (beatAvg > 0) {
      Serial.printf("  HRV (SDNN):        %.1f ms\n", hrv_SDNN);
      Serial.printf("  HRV (RMSSD):       %.1f ms\n", hrv_RMSSD);
      Serial.printf("  HRV Samples:       %d/%d\n", hrvSampleCount, RR_INTERVAL_SIZE);
      
      // Heart rate interpretation
      Serial.print("  HR Interpretation: ");
      if (beatAvg < 60) {
        Serial.println("Low (Resting/Calm)");
      } else if (beatAvg <= 80) {
        Serial.println("Normal (Optimal)");
      } else if (beatAvg <= 100) {
        Serial.println("Elevated (Active)");
      } else {
        Serial.println("High (Stressed/Exercise)");
      }
      
      // HRV interpretation
      Serial.print("  HRV Interpretation: ");
      if (hrv_RMSSD > 50) {
        Serial.println("High (Relaxed/Good recovery)");
      } else if (hrv_RMSSD > 20) {
        Serial.println("Normal");
      } else {
        Serial.println("Low (Stressed/Fatigued)");
      }
    } else {
      Serial.println("  Status: Waiting for stable heart rate...");
      Serial.printf("  Progress: Collecting data (%d beats)\n", beatCount);
    }
    
    // Display last few R-R intervals
    if (hrvSampleCount > 0) {
      Serial.print("  Last R-R Intervals: ");
      int displayCount = min(5, hrvSampleCount);
      for (int i = 0; i < displayCount; i++) {
        int idx = (rrIndex - displayCount + i + RR_INTERVAL_SIZE) % RR_INTERVAL_SIZE;
        Serial.printf("%lu ms", rrIntervals[idx]);
        if (i < displayCount - 1) Serial.print(", ");
      }
      Serial.println();
    }
    
  } else {
    Serial.println("  Status: NO FINGER DETECTED");
    Serial.println("  Action: Place finger firmly on sensor");
  }
  
  // === SYSTEM STATUS ===
  Serial.println("\n[SYSTEM STATUS]");
  Serial.printf("  Current Status:    %s\n", systemStatus.c_str());
  Serial.printf("  WiFi:              %s", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" (IP: %s)", WiFi.localIP().toString().c_str());
  }
  Serial.println();
  Serial.printf("  Free Heap:         %d bytes\n", ESP.getFreeHeap());
  Serial.printf("  ThingSpeak Upload: %s\n", 
                (millis() - lastUploadTime < 20000) ? "Recent" : "Pending");
  
  // === RECOMMENDATIONS ===
  Serial.println("\n[RECOMMENDATIONS]");
  bool hasRecommendation = false;
  
  if (!fingerDetected && max30102OK) {
    Serial.println("  - Place finger on heart rate sensor");
    hasRecommendation = true;
  }
  
  if (fingerDetected && irAC < 30) {
    Serial.println("  - Adjust finger position or increase pressure");
    hasRecommendation = true;
  }
  
  if (fingerDetected && beatCount > 10 && beatAvg == 0) {
    Serial.println("  - Keep finger completely still");
    Serial.println("  - Wait 10-15 more seconds");
    hasRecommendation = true;
  }
  
  if (soundPeakToPeak > 3000) {
    Serial.println("  - Sound sensor gain too high - turn pot counter-clockwise");
    hasRecommendation = true;
  }
  
  if (soundPeakToPeak < 500) {
    Serial.println("  - Sound sensor gain too low - turn pot clockwise");
    hasRecommendation = true;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("  - Check WiFi connection");
    hasRecommendation = true;
  }
  
  if (!hasRecommendation) {
    Serial.println("  All systems operating normally");
  }
  
  Serial.println("\n=======================================================================================\n");
}

// ========== ThingSpeak Upload ==========
void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  WiFiClient client;
  if (!client.connect(thingSpeakServer, 80)) {
    Serial.println("[ThingSpeak] Connection failed");
    return;
  }
  
  String url = "/update?api_key=" + apiKey;
  url += "&field1=" + String(soundPeakToPeak);      // Field 1: Noise_Raw
  url += "&field2=" + String(soundDB, 1);           // Field 2: Noise_Filtered
  url += "&field3=" + String(hrv_RMSSD, 1);         // Field 3: HRV
  url += "&field4=" + String(irValue);              // Field 4: HeartRate_Raw
  url += "&field5=" + String(beatAvg);              // Field 5: Average BPM
  url += "&field6=" + String(beatsPerMinute);       // Field 6: Realtime BPM
  url += "&field7=" + String(fingerDetected ? 1 : 0); // Field 7: Finger Detection
  url += "&field8=" + String(irAC);                 // Field 8: IR AC Component
  url += "&status=" + systemStatus;
  
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + thingSpeakServer + "\r\n" +
               "Connection: close\r\n\r\n");
  
  delay(100);
  client.stop();
  
  Serial.println("[ThingSpeak] Upload successful");
}

// ========== Web Server Handlers ==========
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Smart Study Space</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#f5f5f5;}";
  html += "h1{color:#333;text-align:center;}";
  html += ".container{max-width:800px;margin:0 auto;}";
  html += ".card{background:white;border-radius:10px;padding:20px;margin:15px 0;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  html += ".label{font-size:14px;color:#666;margin-bottom:5px;}";
  html += ".value{font-size:36px;font-weight:bold;color:#4CAF50;margin:10px 0;}";
  html += ".value-small{font-size:24px;font-weight:bold;color:#2196F3;margin:5px 0;}";
  html += ".status-bar{display:flex;justify-content:space-between;align-items:center;padding:10px;background:#e3f2fd;border-radius:5px;}";
  html += ".metric{flex:1;text-align:center;padding:10px;}";
  html += "</style>";
  html += "<script>";
  html += "setInterval(function(){";
  html += "  fetch('/api/data').then(r=>r.json()).then(d=>{";
  html += "    document.getElementById('noise').innerText=d.noise;";
  html += "    document.getElementById('hr_realtime').innerText=d.hr_realtime;";
  html += "    document.getElementById('hr_avg').innerText=d.hr_avg;";
  html += "    document.getElementById('hrv').innerText=d.hrv;";
  html += "    document.getElementById('status').innerText=d.status;";
  html += "    document.getElementById('signal').innerText=d.signal;";
  html += "  });";
  html += "},2000);";
  html += "</script>";
  html += "</head><body>";
  
  html += "<div class='container'>";
  html += "<h1>Smart Study Space Dashboard</h1>";
  
  html += "<div class='card'>";
  html += "<div class='label'>Noise Level</div>";
  html += "<div class='value' id='noise'>" + String(soundDB, 1) + " dB</div>";
  html += "</div>";
  
  html += "<div class='card'>";
  html += "<div class='status-bar'>";
  html += "<div class='metric'>";
  html += "<div class='label'>Real-time HR</div>";
  html += "<div class='value-small' id='hr_realtime'>" + String(beatsPerMinute) + "</div>";
  html += "<div class='label'>bpm</div>";
  html += "</div>";
  html += "<div class='metric'>";
  html += "<div class='label'>Average HR</div>";
  html += "<div class='value-small' id='hr_avg'>" + String(beatAvg) + "</div>";
  html += "<div class='label'>bpm</div>";
  html += "</div>";
  html += "<div class='metric'>";
  html += "<div class='label'>HRV (RMSSD)</div>";
  html += "<div class='value-small' id='hrv'>" + String(hrv_RMSSD, 1) + "</div>";
  html += "<div class='label'>ms</div>";
  html += "</div>";
  html += "</div>";
  
  String signalQuality = "Unknown";
  if (fingerDetected) {
    if (irAC < 30) signalQuality = "WEAK";
    else if (irAC < 100) signalQuality = "FAIR";
    else if (irAC < 500) signalQuality = "GOOD";
    else signalQuality = "EXCELLENT";
  } else {
    signalQuality = "No Finger";
  }
  
  html += "<div class='label' style='text-align:center;margin-top:10px;'>Signal Quality: <span id='signal'>" + signalQuality + "</span></div>";
  html += "</div>";
  
  html += "<div class='card'>";
  html += "<div class='label'>System Status</div>";
  html += "<div class='value' id='status' style='font-size:24px;color:#2196F3;'>" + systemStatus + "</div>";
  html += "<div class='label'>Records: " + String(recordCount) + " | Beats: " + String(beatCount) + "</div>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleData() {
  String signalQuality = "Unknown";
  if (fingerDetected) {
    if (irAC < 30) signalQuality = "WEAK";
    else if (irAC < 100) signalQuality = "FAIR";
    else if (irAC < 500) signalQuality = "GOOD";
    else signalQuality = "EXCELLENT";
  } else {
    signalQuality = "No Finger";
  }
  
  String json = "{";
  json += "\"noise\":" + String(soundDB, 1) + ",";
  json += "\"hr_realtime\":" + String(beatsPerMinute) + ",";
  json += "\"hr_avg\":" + String(beatAvg) + ",";
  json += "\"hrv\":" + String(hrv_RMSSD, 1) + ",";
  json += "\"status\":\"" + systemStatus + "\",";
  json += "\"signal\":\"" + signalQuality + "\",";
  json += "\"p2p\":" + String(soundPeakToPeak) + ",";
  json += "\"ir\":" + String(irValue) + ",";
  json += "\"ir_ac\":" + String(irAC) + ",";
  json += "\"beats\":" + String(beatCount);
  json += "}";
  
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not found");
}
