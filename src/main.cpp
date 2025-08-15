#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// WiFi credentials
const char* ssid = "Frontier:Makerspace";
const char* password = "neverstopbuilding";

// BLE scan parameters
#define SCAN_TIME 5 // Duration of each scan in seconds
#define SCAN_INTERVAL 1000 // Interval between scans in milliseconds
#define MAX_DEVICES 50 // Maximum number of devices to store

// Web server
AsyncWebServer server(80);

// BLE variables
BLEScan* pBLEScan;
int deviceCount = 0;
bool isScanning = false;

// Device storage structure
struct WebBLEDevice {
  String address;
  String name;
  int rssi;
  bool isTracker;
  String trackerType;
  String description;
  String riskLevel;
  unsigned long lastSeen;
  String manufacturerData;
};

// Device storage
WebBLEDevice devices[MAX_DEVICES];
int deviceIndex = 0;
int totalDevices = 0;

// Device classification structure
struct DeviceClassification {
  bool isTracker;
  String trackerType;
  String description;
  String riskLevel;
};

// Function to classify BLE devices
DeviceClassification classifyDevice(BLEAdvertisedDevice advertisedDevice) {
  DeviceClassification classification;
  classification.isTracker = false;
  classification.trackerType = "Unknown";
  classification.description = "Regular BLE device";
  classification.riskLevel = "Low";
  
  std::string manufacturerData = "";
  if (advertisedDevice.haveManufacturerData()) {
    manufacturerData = advertisedDevice.getManufacturerData();
  }
  
  std::string serviceUUID = "";
  if (advertisedDevice.haveServiceUUID()) {
    serviceUUID = advertisedDevice.getServiceUUID().toString();
  }
  
  String deviceName = "";
  if (advertisedDevice.haveName()) {
    deviceName = String(advertisedDevice.getName().c_str());
  }
  
  // Check for Apple devices (manufacturer ID: 4C 00)
  if (manufacturerData.length() >= 2 && 
      (uint8_t)manufacturerData[0] == 0x4C && 
      (uint8_t)manufacturerData[1] == 0x00) {
    
    if (manufacturerData.length() >= 3) {
      uint8_t appleType = (uint8_t)manufacturerData[2];
      
      // AirTag detection - VERY SPECIFIC patterns
      if (appleType == 0x12) {
        if (manufacturerData.length() >= 4) {
          uint8_t subType = (uint8_t)manufacturerData[3];
          // 0x19 = Offline finding network advertisement
          if (subType == 0x19 && manufacturerData.length() >= 25) {
            classification.isTracker = true;
            classification.trackerType = "AirTag (Offline Finding)";
            classification.description = "Apple AirTag in offline finding mode";
            classification.riskLevel = "High";
          }
          // 0x02 = Status advertisement when nearby owner
          else if (subType == 0x02 && manufacturerData.length() == 6) {
            classification.isTracker = true;
            classification.trackerType = "AirTag (Status)";
            classification.description = "Apple AirTag status advertisement";
            classification.riskLevel = "Medium";
          }
        }
      }
      // Find My network accessories (type 0x10) - more specific checks
      else if (appleType == 0x10) {
        // Only classify as tracker if it has the right length and pattern
        if (manufacturerData.length() >= 10 && manufacturerData.length() <= 12) {
          classification.isTracker = true;
          classification.trackerType = "Find My Accessory";
          classification.description = "Apple Find My network accessory";
          classification.riskLevel = "Medium";
        }
      }
      // Remove broad classifications that cause false positives
      // 0x0C (Handoff) and 0x09 (Continuity) are too common and not primarily trackers
    }
  }
  
  // Check for OpenHaystack devices (very specific service UUIDs)
  if (serviceUUID.find("6ba1b218-15a8-461f-9fa8-5dcae2e8cd51") != std::string::npos ||
      serviceUUID.find("19b10000-e8f2-537e-4f6c-d104768a1214") != std::string::npos) {
    classification.isTracker = true;
    classification.trackerType = "OpenHaystack";
    classification.description = "OpenHaystack DIY tracker";
    classification.riskLevel = "Medium";
  }
  
  // Check for Tile trackers - more specific detection
  if (serviceUUID.find("0000feed-0000-1000-8000-00805f9b34fb") != std::string::npos) {
    classification.isTracker = true;
    classification.trackerType = "Tile Tracker";
    classification.description = "Tile Bluetooth tracker";
    classification.riskLevel = "Medium";
  }
  
  // Check for Samsung Galaxy SmartTag - more specific pattern
  if (manufacturerData.length() >= 26 && 
      (uint8_t)manufacturerData[0] == 0x75 && 
      (uint8_t)manufacturerData[1] == 0x00 &&
      (uint8_t)manufacturerData[2] == 0x42 &&
      (uint8_t)manufacturerData[3] == 0x04) {
    classification.isTracker = true;
    classification.trackerType = "Samsung SmartTag";
    classification.description = "Samsung Galaxy SmartTag";
    classification.riskLevel = "Medium";
  }
  
  // Check for specific tracker device names (avoid false positives)
  if (deviceName.equals("AirTag") || 
      deviceName.startsWith("Tile_") ||
      deviceName.equals("SmartTag") ||
      deviceName.startsWith("Galaxy SmartTag")) {
    classification.isTracker = true;
    classification.trackerType = "Named Tracker";
    classification.description = "Device with tracker name";
    classification.riskLevel = "Medium";
  }
  
  // Additional check for known fitness trackers to avoid false positives
  if (deviceName.indexOf("Fitbit") != -1 || 
      deviceName.indexOf("Garmin") != -1 ||
      deviceName.indexOf("Amazfit") != -1 ||
      deviceName.indexOf("Band") != -1 ||
      deviceName.indexOf("Watch") != -1) {
    // These are fitness trackers, not location trackers
    classification.isTracker = false;
    classification.trackerType = "Fitness Device";
    classification.description = "Fitness tracker or smartwatch";
    classification.riskLevel = "Low";
  }
  
  return classification;
}

// Function to find existing device by address
int findDeviceIndex(String address) {
  for (int i = 0; i < totalDevices; i++) {
    if (devices[i].address == address) {
      return i;
    }
  }
  return -1;
}

// Function to add or update device
void addOrUpdateDevice(BLEAdvertisedDevice advertisedDevice) {
  String address = String(advertisedDevice.getAddress().toString().c_str());
  int existingIndex = findDeviceIndex(address);
  
  DeviceClassification classification = classifyDevice(advertisedDevice);
  
  // Prepare manufacturer data string
  String manufacturerDataStr = "";
  if (advertisedDevice.haveManufacturerData()) {
    std::string manufacturerData = advertisedDevice.getManufacturerData();
    for (int i = 0; i < manufacturerData.length(); i++) {
      if (i > 0) manufacturerDataStr += " ";
      manufacturerDataStr += String((uint8_t)manufacturerData[i], HEX);
      if (manufacturerDataStr.length() > 1) {
        manufacturerDataStr.toUpperCase();
      }
    }
  }
  
  WebBLEDevice device;
  device.address = address;
  device.name = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : "";
  device.rssi = advertisedDevice.getRSSI();
  device.isTracker = classification.isTracker;
  device.trackerType = classification.trackerType;
  device.description = classification.description;
  device.riskLevel = classification.riskLevel;
  device.lastSeen = millis();
  device.manufacturerData = manufacturerDataStr;
  
  if (existingIndex >= 0) {
    // Update existing device
    devices[existingIndex] = device;
  } else {
    // Add new device
    if (totalDevices < MAX_DEVICES) {
      devices[totalDevices] = device;
      totalDevices++;
    } else {
      // Replace oldest device (circular buffer)
      devices[deviceIndex] = device;
      deviceIndex = (deviceIndex + 1) % MAX_DEVICES;
    }
  }
}

// Callback class to handle discovered BLE devices
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      deviceCount++;
      
      // Add device to storage
      addOrUpdateDevice(advertisedDevice);
      
      // Classify the device
      DeviceClassification classification = classifyDevice(advertisedDevice);
      
      Serial.println("========================================");
      Serial.printf("Device #%d", deviceCount);
      if (classification.isTracker) {
        Serial.printf(" 🚨 TRACKER DETECTED");
      }
      Serial.println();
      Serial.println("========================================");
      
      // Device Address (MAC)
      Serial.printf("Address: %s\n", advertisedDevice.getAddress().toString().c_str());
      
      // Device Name
      if (advertisedDevice.haveName()) {
        Serial.printf("Name: %s\n", advertisedDevice.getName().c_str());
      } else {
        Serial.println("Name: [Unknown]");
      }
      
      // Signal Strength (RSSI)
      Serial.printf("RSSI: %d dBm\n", advertisedDevice.getRSSI());
      
      // TRACKER CLASSIFICATION SECTION
      Serial.println("--- TRACKER ANALYSIS ---");
      Serial.printf("Is Tracker: %s\n", classification.isTracker ? "YES" : "NO");
      if (classification.isTracker) {
        Serial.printf("Tracker Type: %s\n", classification.trackerType.c_str());
        Serial.printf("Description: %s\n", classification.description.c_str());
        Serial.printf("Risk Level: %s\n", classification.riskLevel.c_str());
        
        // Additional warning for high-risk trackers
        if (classification.riskLevel == "High") {
          Serial.println("⚠️  WARNING: This device may be tracking your location!");
        }
      }
      Serial.println("---------------------------");
      
      // Device Type
      if (advertisedDevice.haveAppearance()) {
        Serial.printf("Appearance: 0x%04X\n", advertisedDevice.getAppearance());
      }
      
      // Manufacturer Data
      if (advertisedDevice.haveManufacturerData()) {
        std::string manufacturerData = advertisedDevice.getManufacturerData();
        Serial.printf("Manufacturer Data: ");
        for (int i = 0; i < manufacturerData.length(); i++) {
          Serial.printf("%02X ", (uint8_t)manufacturerData[i]);
        }
        Serial.println();
      }
      
      // Service UUIDs
      if (advertisedDevice.haveServiceUUID()) {
        Serial.printf("Service UUID: %s\n", advertisedDevice.getServiceUUID().toString().c_str());
      }
      
      // TX Power
      if (advertisedDevice.haveTXPower()) {
        Serial.printf("TX Power: %d dBm\n", advertisedDevice.getTXPower());
      }
      
      // Service Data
      if (advertisedDevice.haveServiceData()) {
        std::string serviceData = advertisedDevice.getServiceData();
        Serial.printf("Service Data: ");
        for (int i = 0; i < serviceData.length(); i++) {
          Serial.printf("%02X ", (uint8_t)serviceData[i]);
        }
        Serial.println();
      }
      
      Serial.println("----------------------------------------");
      Serial.println();
    }
};

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("========================================");
  Serial.println("    ESP32 BLE Tracker Scanner Web      ");
  Serial.println("========================================");
  Serial.println();
  
  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  
  // Connect to WiFi
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Web interface: http://%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed!");
  }
  
  // Initialize BLE
  BLEDevice::init("");
  
  // Create BLE Scanner
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  
  // Set scan parameters
  pBLEScan->setActiveScan(true); // Active scan uses more power but gets more info
  pBLEScan->setInterval(100);    // Scan interval in milliseconds
  pBLEScan->setWindow(99);       // Scan window in milliseconds
  
  // Setup web server routes
  
  // Serve main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });
  
  // API endpoint to get devices
  server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request){
    JsonDocument doc;
    JsonArray deviceArray = doc["devices"].to<JsonArray>();
    
    for (int i = 0; i < totalDevices; i++) {
      JsonObject deviceObj = deviceArray.add<JsonObject>();
      deviceObj["address"] = devices[i].address;
      deviceObj["name"] = devices[i].name;
      deviceObj["rssi"] = devices[i].rssi;
      deviceObj["isTracker"] = devices[i].isTracker;
      deviceObj["trackerType"] = devices[i].trackerType;
      deviceObj["description"] = devices[i].description;
      deviceObj["riskLevel"] = devices[i].riskLevel;
      deviceObj["lastSeen"] = devices[i].lastSeen;
      deviceObj["manufacturerData"] = devices[i].manufacturerData;
    }
    
    JsonObject status = doc["status"].to<JsonObject>();
    status["wifi"] = WiFi.status() == WL_CONNECTED;
    status["ip"] = WiFi.localIP().toString();
    status["scanning"] = isScanning;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // API endpoint to trigger scan
  server.on("/api/scan", HTTP_POST, [](AsyncWebServerRequest *request){
    // Trigger a new scan in the main loop
    request->send(200, "application/json", "{\"status\":\"scan_triggered\"}");
  });
  
  // API endpoint for system status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    JsonDocument doc;
    doc["wifi"] = WiFi.status() == WL_CONNECTED;
    doc["ip"] = WiFi.localIP().toString();
    doc["scanning"] = isScanning;
    doc["deviceCount"] = totalDevices;
    doc["uptime"] = millis();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });
  
  // Start server
  server.begin();
  Serial.println("Web server started!");
  
  Serial.println("BLE Scanner initialized successfully!");
  Serial.printf("Scan duration: %d seconds\n", SCAN_TIME);
  Serial.printf("Scan interval: %d ms\n", SCAN_INTERVAL);
  Serial.println();
  Serial.println("Starting BLE device discovery...");
  Serial.println();
}

void loop() {
  deviceCount = 0; // Reset device counter for each scan
  isScanning = true;
  
  Serial.println("🔍 Starting new BLE scan...");
  Serial.println();
  
  // Start BLE scan
  BLEScanResults foundDevices = pBLEScan->start(SCAN_TIME, false);
  
  isScanning = false;
  
  Serial.println("========================================");
  Serial.printf("Scan completed! Found %d device(s)\n", foundDevices.getCount());
  Serial.printf("Total stored devices: %d\n", totalDevices);
  Serial.println("========================================");
  Serial.println();
  
  // Clear scan results to free memory
  pBLEScan->clearResults();
  
  // Wait before next scan
  Serial.printf("Waiting %d ms before next scan...\n", SCAN_INTERVAL);
  Serial.println();
  delay(SCAN_INTERVAL);
}
