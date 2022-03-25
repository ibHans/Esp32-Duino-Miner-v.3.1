/* Duco Miner Ver.3.1 */
/* Without OTA and Webserver */
const char *SSID = "My cool Wi-Fi";           // Change the part in brackets to your WiFi name
const char *WIFI_PASS = "My secret pass";     // Change the part in brackets to your WiFi pasword
const char *DUCO_USER = "my_cool_username";   // Change the part in brackets to your Duco account
const char *RIG_IDENTIFIER = "Auto";          // Change the part in brackets to your Rig name
const char* MINER_KEY = "None";               // Change the part in brackets to your Miner key
#define LED_BUILTIN 2
#define BLINK_SHARE_FOUND    1
#define BLINK_SETUP_COMPLETE 2
#define BLINK_CLIENT_CONNECT 3
#define BLINK_RESET_DEVICE   5
const bool LED_BLINKING = true;
#define WDT_TIMEOUT 60
#pragma GCC optimize ("-Ofast")
#include "sha/sha_parallel_engine.h"
const char *mqtt_server = "";
const int mqtt_port = 1883;
#define ENABLE_SERIAL
/* ***************** END OF MINER CONFIGURATION SECTION ***************** */
#ifndef ENABLE_SERIAL
#define Serial DummySerial
static class {
  public:
    void begin(...) {}
    void print(...) {}
    void println(...) {}
    void printf(...) {}
} Serial;
#endif

#ifndef ENABLE_MQTT
#define PubSubClient DummyPubSubClient
class PubSubClient {
  public:
    PubSubClient(Client& client) {}
    bool connect(...) {
      return false;
    }
    bool connected(...) {
      return true;
    }
    void loop(...) {}
    void publish(...) {}
    void subscribe(...) {}
    void setServer(...) {}
};
#endif

// Data Structures
typedef struct TaskData {
  TaskHandle_t handler;
  byte taskId;
  float hashrate;
  unsigned long shares;
  unsigned int difficulty;

} TaskData_t;

// Include required libraries
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>

#ifdef ENABLE_MQTT
#include <PubSubClient.h>
#endif

// Global Definitions
#define NUMBEROFCORES 2
#define MSGDELIMITER ','
#define MSGNEWLINE '\n'

// Handles for additional threads
TaskHandle_t WiFirec;
TaskData_t TaskThreadData[NUMBEROFCORES];
TaskHandle_t MqttPublishHandle;
SemaphoreHandle_t xMutex;

const char * DEVICE = "ESP32";
const char * POOLPICKER_URL[] = {"https://server.duinocoin.com/getPool"};
const char * MINER_BANNER = "Official ESP32 Miner";
const char * MINER_VER = "3.1";
String pool_name = "";
String host = "";
String node_id = "";
int port = 0;
int walletid = 0;
volatile int wifi_state = 0;
volatile int wifi_prev_state = WL_CONNECTED;
volatile bool ota_state = false;
volatile char chip_id[23];
char rigname_auto[23];
int mqttUpdateTrigger = 0;
String mqttRigTopic = "";

WiFiClient wifiMqttClient;
PubSubClient mqttClient(wifiMqttClient);

// Util Functions
void blink(uint8_t count, uint8_t pin = LED_BUILTIN) {
  if (LED_BLINKING)
  {
    uint8_t state = LOW;

    for (int x = 0; x < (count << 1); ++x) {
      digitalWrite(pin, state ^= HIGH);
      delay(75);
    }
  }
}

String httpGetString(String URL) {
  String payload = "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (http.begin(client, URL)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      payload = http.getString();
    } else {
      Serial.printf("Poolpicker fetch failed - error: %s\n",
                    http.errorToString(httpCode).c_str());
    }
    http.end();
  }
  return payload;
}

void UpdateHostPort(String input) {
  // Thanks @ricaun for the code
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, input);

  if (error) {
    Serial.print(F("JSON deserialization failed - error: "));
    Serial.println(error.f_str());
    return;
  }

  const char *name = doc["name"];
  const char *h = doc["ip"];
  int p = doc["port"];

  host = h;
  port = p;
  node_id = String(name);

  // Send to MQTT
  mqttClient.publish((mqttRigTopic + "pool_name").c_str(), name);
  mqttClient.publish((mqttRigTopic + "pool_ip").c_str(), h);
  mqttClient.publish((mqttRigTopic + "pool_port").c_str(), String(p).c_str());

  // Send to Serial
  Serial.println("Poolpicker selected the best mining node: " + String(name));
}

// Communication Functions
void UpdatePool() {
  String input = "";
  int waitTime = 1;
  int poolIndex = 0;
  int poolSize = sizeof(POOLPICKER_URL) / sizeof(char*);

  while (input == "") {
    Serial.println("Fetching mining node from the poolpicker in " + String(waitTime) + "s");
    input = httpGetString(POOLPICKER_URL[poolIndex]);
    poolIndex += 1;

    // Check if pool index needs to roll over
    if (poolIndex >= poolSize) {
      poolIndex %= poolSize;
      delay(waitTime * 500);

      // Increase wait time till a maximum of 16 seconds (addresses: Limit connection requests on failure in ESP boards #1041)
      waitTime *= 2;
      if ( waitTime > 16 )
        waitTime = 16;
    }
  }

  // Setup pool with new input
  UpdateHostPort(input);
}

void HandleMqttConnection() {
  // Check Connection
  if (!mqttClient.connected()) {
    // Setup MQTT Client
    Serial.println("Connecting to MQTT server: " + String(mqtt_server) + " on port: " + String(mqtt_port));
    mqttClient.setServer(mqtt_server, mqtt_port);

    // Setup Rig Topic
    mqttRigTopic = "duinocoin/" + String(RIG_IDENTIFIER) + "/";

    // Try to connect
    if (mqttClient.connect(RIG_IDENTIFIER, (mqttRigTopic + "state").c_str(), 0, true, String(0).c_str())) {
      // Connection Succesfull
      Serial.println("Succesfully connected to MQTT server");

      // Output connection info
      mqttClient.publish((mqttRigTopic + "ip").c_str(), WiFi.localIP().toString().c_str());
      mqttClient.publish((mqttRigTopic + "name").c_str(), String(RIG_IDENTIFIER).c_str());
    }
    else {
      // Connection Failed
      Serial.println("Failed to connect to MQTT server");
    }
  }

  // Default MQTT Loop
  mqttClient.loop();
}

void WiFireconnect(void *pvParameters) {
  int n = 0;
  unsigned long previousMillis = 0;
  const long interval = 500;
  esp_task_wdt_add(NULL);
  for (;;) {
    wifi_state = WiFi.status();

    // check if WiFi status has changed.
    if ((wifi_state == WL_CONNECTED) && (wifi_prev_state != WL_CONNECTED)) {
      esp_task_wdt_reset();  // Reset watchdog timer

      // Connect to MQTT (will do nothing if MQTT is disabled)
      HandleMqttConnection();

      // Write Data to Serial
      Serial.println("\n\nSuccessfully connected to WiFi");
      Serial.println("Local IP address: " + WiFi.localIP().toString());
      Serial.println("Rig name: " + String(RIG_IDENTIFIER));
      Serial.println();

      // Notify Setup Complete
      blink(BLINK_SETUP_COMPLETE);// Sucessfull connection with wifi network

      // Update Pool and wait a bit
      UpdatePool();
      yield();
      delay(100);
    }

    else if ((wifi_state != WL_CONNECTED) &&
             (wifi_prev_state == WL_CONNECTED)) {
      esp_task_wdt_reset();  // Reset watchdog timer
      Serial.println(F("\nWiFi disconnected!"));
      WiFi.disconnect();

      Serial.println(F("Scanning for WiFi networks"));
      n = WiFi.scanNetworks(false, true);

      if (n == 0) {
        Serial.println(F("No networks found - restarting..."));
        blink(BLINK_RESET_DEVICE);
        esp_restart();
      }
      else {
        Serial.print(n);
        Serial.println(F(" networks found"));
        for (int i = 0; i < n; ++i) {
          // Print SSID and RSSI for each network found
          Serial.print(i + 1);
          Serial.print(F(": "));
          Serial.print(WiFi.SSID(i));
          Serial.print(F(" ("));
          Serial.print(WiFi.RSSI(i));
          Serial.print(F(")"));
          Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " "
                         : "*");
          delay(10);
        }
      }

      esp_task_wdt_reset();  // Reset watchdog timer
      Serial.println();
      Serial.println(
        F("Please check if your WiFi network is on the list and check if "
          "it's strong enough (greater than -90)"));
      Serial.println("ESP32 will reset itself after " + String(WDT_TIMEOUT) +
                     " seconds if can't connect to the network");

      Serial.print("Connecting to: " + String(SSID));
      WiFi.reconnect();
    }

    else if ((wifi_state == WL_CONNECTED) &&
             (wifi_prev_state == WL_CONNECTED)) {
      esp_task_wdt_reset();  // Reset watchdog timer
      delay(1000);
    }

    else {
      // Don't reset watchdog timer
      unsigned long currentMillis = millis();
      if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;
        Serial.print(F("."));
      }
    }
    wifi_prev_state = wifi_state;
  }
}

// Miner Code
void TaskMining(void *pvParameters) {
  // Setup Thread
  esp_task_wdt_add(NULL); // Disable watchdogtimer for this thread

  // Setup thread data
  String taskCoreName = "Core " + String(xPortGetCoreID());
  int taskId = xPortGetCoreID();

  // Start main thread loop
  for (;;) {
    // If OTA needs to be preformed reset the task watchdog
    if (ota_state)
      esp_task_wdt_reset();

    // Wait for a valid network connection
    while (wifi_state != WL_CONNECTED) {
      delay(500);
      esp_task_wdt_reset();
    }

    // Wait for server to get pool information
    while (port == 0) {
      Serial.println(String(taskCoreName + " is waiting for the poolpicker..."));
      delay(1000);
      esp_task_wdt_reset();
    }

    // Setup WiFi Client and connection details
    Serial.println("\n\n" + String(taskCoreName) + " is connecting to the Duino-Coin server...");
    WiFiClient jobClient;
    jobClient.setTimeout(1);
    jobClient.flush();
    yield();

    // Start connection to Duino-Coin server
    if (!jobClient.connect(host.c_str(), port)) {
      Serial.println(String(taskCoreName + " failed to connect"));
      delay(500);
      continue;
    }

    // Wait for server connection
    Serial.println(String(taskCoreName + " has connected to the server"));
    while (!jobClient.available()) {
      yield();
      if (!jobClient.connected()) break;
      delay(10);
    }

    // Server sends SERVER_VERSION after connecting
    String SERVER_VER = jobClient.readStringUntil(MSGNEWLINE);
    Serial.println(String(taskCoreName + " received server version: v" + SERVER_VER));
    blink(BLINK_CLIENT_CONNECT); // Sucessfull connection with the server

    // Define job loop variables
    int jobClientBufferSize = 0;

    // Start Job loop
    while (jobClient.connected()) {
      // Reset watchdog timer before each job
      esp_task_wdt_reset();

      // We are connected and are able to request a job
      Serial.println(String(taskCoreName + " asking for a new job for user: " + DUCO_USER));
      jobClient.flush();
      jobClient.print("JOB," + String(DUCO_USER) + ",ESP32," + String(MINER_KEY) + MSGNEWLINE);
      while (!jobClient.available()) {
        if (!jobClient.connected()) break;
        delay(10);
      }
      yield();

      // Check buffer size
      jobClientBufferSize = jobClient.available();
      if (jobClientBufferSize <= 64) {
        Serial.println(String(taskCoreName + " received an invalid job with size of " + jobClientBufferSize + " bytes - requesting a new job..."));
        continue;
      }
      else {
        Serial.println(String(taskCoreName + " received a correct job with size of " + jobClientBufferSize + " bytes"));
      }

      // Read hash, expected hash and difficulty from job description
      String previousHash = jobClient.readStringUntil(MSGDELIMITER);
      String expectedHash = jobClient.readStringUntil(MSGDELIMITER);
      TaskThreadData[taskId].difficulty = jobClient.readStringUntil(MSGNEWLINE).toInt() * 100;
      jobClient.flush();
      if (LED_BLINKING) digitalWrite(LED_BUILTIN, LOW);

      // Global Definitions
      unsigned int job_size_task_one = 100;
      unsigned char expectedHashBytes[100];

      // Clear expectedHashBytes
      memset(expectedHashBytes, 0, job_size_task_one);
      size_t expectedHashLength = expectedHash.length() / 2;

      // Convert expected hash to byte array (for easy comparison)
      const char *cExpectedHash = expectedHash.c_str();
      for (size_t i = 0, j = 0; j < expectedHashLength; i += 2, j++)
        expectedHashBytes[j] = (cExpectedHash[i] % 32 + 9) % 25 * 16 + (cExpectedHash [i + 1] % 32 + 9) % 25;

      // Start measurement
      unsigned long startTime = micros();
      byte shaResult[20];
      String hashUnderTest;
      unsigned int hashUnderTestLength;
      bool ignoreHashrate = false;

      // Try to find the nonce which creates the expected hash
      for (unsigned long nonceCalc = 0; nonceCalc <= TaskThreadData[taskId].difficulty; nonceCalc++) {
        // Define hash under Test
        hashUnderTest = previousHash + String(nonceCalc);
        hashUnderTestLength = hashUnderTest.length();

        // Wait for hash module lock
        while ( xSemaphoreTake(xMutex, portMAX_DELAY) != pdTRUE );

        // We are allowed to perform our hash
        esp_sha(SHA1, (const unsigned char *)hashUnderTest.c_str(), hashUnderTestLength, shaResult);

        // Release hash module lock
        xSemaphoreGive(xMutex);

        // Check if we have found the nonce for the expected hash
        if ( memcmp( shaResult, expectedHashBytes, sizeof(shaResult) ) == 0 ) {
          // Found the nonce submit it to the server
          Serial.println(String(taskCoreName + " found a correct hash using nonce: " + nonceCalc ));

          // Calculate mining time
          float elapsedTime = (micros() - startTime) / 1000.0 / 1000.0; // Total elapsed time in seconds
          TaskThreadData[taskId].hashrate = nonceCalc / elapsedTime;

          // Validate connection
          if (!jobClient.connected()) {
            Serial.println(String(taskCoreName + " lost connection - reconnecting..."));
            if (!jobClient.connect(host.c_str(), port)) {
              Serial.println(String(taskCoreName + " failed to connect"));
              break;
            }
            Serial.println(String(taskCoreName + " reconnected successfully"));
          }

          // Send result to server
          jobClient.flush();
          jobClient.print(
            String(nonceCalc) + MSGDELIMITER + String(TaskThreadData[taskId].hashrate) + MSGDELIMITER +
            String(MINER_BANNER) + " " + String(MINER_VER) + MSGDELIMITER + String(RIG_IDENTIFIER) + MSGDELIMITER +
            "DUCOID" + String((char *)chip_id) + MSGDELIMITER + String(walletid) + MSGNEWLINE);
          jobClient.flush();

          // Wait for job result
          while (!jobClient.available()) {
            if (!jobClient.connected()) {
              Serial.println(String(taskCoreName + " lost connection and didn't receive feedback"));
              break;
            }
            delay(10);
          }
          yield();

          // Handle feedback
          String feedback = jobClient.readStringUntil(MSGNEWLINE);
          jobClient.flush();
          TaskThreadData[taskId].shares++;
          if (LED_BLINKING) digitalWrite(LED_BUILTIN, HIGH);

          // Validate Hashrate
          if ( TaskThreadData[taskId].hashrate < 4000 && !ignoreHashrate) {
            // Hashrate is low so restart esp
            Serial.println(String(taskCoreName + " has low hashrate: " + (TaskThreadData[taskId].hashrate / 1000) + "kH/s, job feedback: " + feedback + " - restarting..."));
            jobClient.flush();
            jobClient.stop();
            blink(BLINK_RESET_DEVICE);
            esp_restart();
          }
          else {
            // Print statistics
            Serial.println(String(taskCoreName + " retrieved job feedback: " + feedback + ", hashrate: " + (TaskThreadData[taskId].hashrate / 1000) + "kH/s, share #" + TaskThreadData[taskId].shares));
          }

          // Stop current loop and ask for a new job
          break;
        }
      }
    }

    Serial.println(String(taskCoreName + " is not connected - restarting..."));
    jobClient.flush();
    jobClient.stop();
  }
}

void setup() {
  Serial.begin(500000);  // Start serial connection
  Serial.println("\n\nDuino-Coin " + String(MINER_BANNER));

  WiFi.mode(WIFI_STA);  // Setup ESP in client mode
  btStop();
  WiFi.begin(SSID, WIFI_PASS);  // Connect to wifi

  uint64_t chipid = ESP.getEfuseMac();  // Getting chip chip_id
  uint16_t chip =
    (uint16_t)(chipid >> 32);  // Preparing for printing a 64 bit value (it's
  // actually 48 bits long) into a char array
  snprintf(
    (char *)chip_id, 23, "%04X%08X", chip,
    (uint32_t)chipid);  // Storing the 48 bit chip chip_id into a char array.
  walletid = random(0, 2811);

  // Autogenerate ID if required
  if ( strcmp(RIG_IDENTIFIER, "Auto") == 0 ) {
    snprintf(rigname_auto, 23, "ESP32-%04X%08X", chip, (uint32_t)chipid);
    RIG_IDENTIFIER = &rigname_auto[0];
  }

  esp_task_wdt_init(WDT_TIMEOUT, true);  // Init Watchdog timer
  pinMode(LED_BUILTIN, OUTPUT);

  // Determine which cores to use
  int wifiCore = 0;
  int mqttCore = 0;
  if ( NUMBEROFCORES >= 2 ) mqttCore = 1;

  // Create Semaphore and main Wifi Monitoring Thread
  xMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
    WiFireconnect, "WiFirec", 10000, NULL, NUMBEROFCORES + 2, &WiFirec,
    mqttCore);  // create a task with highest priority and executed on core 0
  delay(250);

  // If MQTT is enabled create a sending thread
  #ifdef ENABLE_MQTT
  Serial.println("Creating mqtt thread on core: " + String(mqttCore));
  xTaskCreatePinnedToCore(
    MqttPublishCode, "MqttPublishCode", 10000, NULL, 1, &MqttPublishHandle,
    mqttCore); //create a task with lowest priority and executed on core 1
  delay(250);
  #endif

  // Create Mining Threads
  for ( int i = 0; i < NUMBEROFCORES; i++ ) {
    Serial.println("Creating mining thread on core: " + String(i));
    xTaskCreatePinnedToCore(
      TaskMining, String("Task" + String(i)).c_str(), 10000, NULL, 2 + i, &TaskThreadData[NUMBEROFCORES].handler,
      i);  // create a task with priority 2 (+ core id) and executed on a specific core
    delay(250);
  }
}

// ************************************************************
void MqttPublishCode( void * pvParameters ) {
  unsigned long lastWdtReset = 0;
  unsigned long wdtResetDelay = 30000;

  for (;;) {
    if ((millis() - lastWdtReset) > wdtResetDelay) {
      // Reset timers
      esp_task_wdt_reset();
      lastWdtReset = millis();

      // Calculate combined hashrate and average difficulty
      float avgDiff = 0.0;
      float totHash = 0.0;
      unsigned long totShares = 0;
      for (int i = 0; i < NUMBEROFCORES; i++) {
        avgDiff += TaskThreadData[i].difficulty;
        totHash += TaskThreadData[i].hashrate;
        totShares += TaskThreadData[i].shares;
      }
      avgDiff /= NUMBEROFCORES;

      // Update States
      mqttClient.publish((mqttRigTopic + "state").c_str(), String(1).c_str());
      mqttClient.publish((mqttRigTopic + "hashrate").c_str(), String(totHash).c_str());
      mqttClient.publish((mqttRigTopic + "avgdiff").c_str(), String(avgDiff).c_str());
      mqttClient.publish((mqttRigTopic + "shares").c_str(), String(totShares).c_str());
    }

    mqttClient.loop();
    yield();
  }
}

void loop() {
  vTaskDelete(NULL); /* Free up The CPU */
}
