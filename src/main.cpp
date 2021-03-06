#include <FS.h>
#include <LittleFS.h>
#include <Arduino.h>
#include <ArduinoLog.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <CircularBuffer.h>

#include "Version.h"
#include "config.h"
#include "Shutter.hpp"

Shutter shutter1("left");
Shutter shutter2("right");

bool shouldSaveConfig = false;
char mqttServer[40] = "";
char mqttPort[6] = "1883";
char mqttUser[40] = "";
char mqttPassword[40] = "";
char shutterDelay[5] = "1500";

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
WiFiEventHandler wifiGotIpHandler;
String clientId;
WiFiClient wifiClient;

PubSubClient mqttClient(wifiClient);
unsigned long mqttLastReconnectAttempt = 0;

typedef struct {
    String topic;
    String payLoad;
} mqttRecord_t;

CircularBuffer<mqttRecord_t, 10> mqttQueue;
bool suppressQueueLogMessage = false;

enum MqttMode {
    INVALID_MQTT_MODE = -100,
    GLOBAL = -1,
    DEVICE = 0,
    SHUTTER1 = 1,
    SHUTTER2 = 2,
};

Ticker onboardLedBlinker;

void blinkOnboardLed() {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

void startBlinkOnboardLed(bool critical = false) {
    if (!onboardLedBlinker.active()) {
        onboardLedBlinker.attach_ms(critical ? 200 : 800, blinkOnboardLed);
    }
}

void stopBlinkOnboardLed() {
    if (onboardLedBlinker.active()) {
        onboardLedBlinker.detach();
        digitalWrite(LED_BUILTIN, LED_ONBOARD_ACTIVE ? LOW : HIGH);
    }
}

void printTimestamp(Print* _logOutput) {
    char c[12];
    sprintf(c, "%10lu ", millis());
    _logOutput->print(c);
}

void printNewline(Print* _logOutput) {
    _logOutput->print('\n');
}

String buildMqttTopic (String subTopic, MqttMode mqttMode) {
    String mqttTopic;
    
    if (mqttMode == MqttMode::GLOBAL) {
        mqttTopic = String(CLIENT_ID_PREFIX) + "s/";
    } else {
        mqttTopic = clientId + "/";
    }

    if (mqttMode > MqttMode::DEVICE) {
        mqttTopic += "shutter" + String(mqttMode) + "/";    
    }
    mqttTopic += subTopic;

    return mqttTopic;
}

void subscribeMqttTopic(String topic) {
    Log.notice("[ %s:%d ] Subcribe to MQTT topic [ %s ].", __FILE__, __LINE__, topic.c_str());
    mqttClient.subscribe(topic.c_str());
}

void publishMqttTopic(String topic, String payload, bool retain = false) {
    Log.notice("[ %s:%d ] Publish MQTT topic [ %s ] with payload [ %s ] and retain [ %s ].", __FILE__, __LINE__, topic.c_str(), payload.c_str(), retain ? "true" : "false");
    mqttClient.publish(topic.c_str(), payload.c_str(), retain);
}

void sendStatusShutter1Mqtt() {
    publishMqttTopic(buildMqttTopic("state", MqttMode::SHUTTER1), shutter1.getStatus(), true);
    publishMqttTopic(buildMqttTopic("position", MqttMode::SHUTTER1), String(shutter1.getPosition()), true);
}

void sendStatusShutter2Mqtt() {
    publishMqttTopic(buildMqttTopic("state", MqttMode::SHUTTER2), shutter2.getStatus(), true);
    publishMqttTopic(buildMqttTopic("position", MqttMode::SHUTTER2), String(shutter2.getPosition()), true);
}

void announceMqtt() {
    publishMqttTopic(buildMqttTopic("availability", MqttMode::DEVICE), "online", true);
        
    sendStatusShutter1Mqtt();
    sendStatusShutter2Mqtt();
}

String convertPayload(byte* payload, unsigned int length) {
    String strPayload = "";

    for (uint i = 0; i < length; i++) {
        strPayload += (char)payload[i];
    }

    return strPayload;
}

int getPositionFromPayload(String payload) {
    int position = -1;
    bool isNum = true;

    payload.trim();
    for(byte i = 0; i < payload.length(); i++) {
        isNum = isDigit(payload.charAt(i));
        if (!isNum) {
            break;
        }
    }

    if (isNum) {
        position = payload.toInt();
        if (position < 0 || position > 100) {
            position = -1;
        }
    }

    return position;
}

MqttMode getMqttModeFromTopic(String topic) {
    MqttMode mqttMode = MqttMode::INVALID_MQTT_MODE;
    
    if (topic.startsWith(buildMqttTopic("", MqttMode::SHUTTER1))) {
        mqttMode = MqttMode::SHUTTER1;
    } else if (topic.startsWith(buildMqttTopic("", MqttMode::SHUTTER2))) {
        mqttMode = MqttMode::SHUTTER2;
    } else if (topic.startsWith(buildMqttTopic("", MqttMode::DEVICE))) {
        mqttMode = MqttMode::DEVICE;
    } else if (topic.startsWith(buildMqttTopic("", MqttMode::GLOBAL))) {
        mqttMode = MqttMode::GLOBAL;
    } 

    return mqttMode;
}

ShutterAction getShutterActionFromPayload(String payload) {
    ShutterAction shutterAction = ShutterAction::UNDEFINED_ACTION;

    if (payload.equalsIgnoreCase("down")) {
        shutterAction = ShutterAction::DOWN;
    } else if (payload.equalsIgnoreCase("stop")) {
        shutterAction = ShutterAction::STOP;
    } else if (payload.equalsIgnoreCase("up")) {
        shutterAction = ShutterAction::UP;
    }

    return shutterAction;
}

void workMqttMessage(mqttRecord_t mqttRec) {
    MqttMode mqttMode = getMqttModeFromTopic(mqttRec.topic);
    bool isValid = false;
    int position = -1;
    ShutterAction shutterAction = ShutterAction::UNDEFINED_ACTION;

    Log.notice("[ %s:%d ] MQTT message dequeued with topic [ %s ] and payload [ %s ].", __FILE__, __LINE__, mqttRec.topic.c_str(), mqttRec.payLoad.c_str());

    if (mqttMode != MqttMode::INVALID_MQTT_MODE) {
        if (mqttMode == MqttMode::SHUTTER1 ||
            mqttMode == MqttMode::SHUTTER2) {
            if (mqttRec.topic.endsWith("set_position")) {
                position = getPositionFromPayload(mqttRec.payLoad);
                if (position >= 0) {
                    shutterAction = ShutterAction::MOVE_BY_POSITION;
                    isValid = true;
                }
            } else if (mqttRec.topic.endsWith("set")) {
                shutterAction = getShutterActionFromPayload(mqttRec.payLoad);
                if (shutterAction != ShutterAction::UNDEFINED_ACTION) {
                    isValid = true;
                }
            }

            if (isValid) {
                if (mqttMode == MqttMode::SHUTTER1) {
                    shutter1.executeAction(shutterAction, position);
                } else if (mqttMode == MqttMode::SHUTTER2) {
                    shutter2.executeAction(shutterAction, position);
                }                
            }
        } else if (mqttMode == MqttMode::GLOBAL) {
            if (mqttRec.payLoad == "announce") {
                isValid = true;
                announceMqtt();
             }
        }
    }

    if (!isValid) {
        Log.warning("[ %s:%d ] MQTT message cannot be processed, most likly incorrect topic [ %s ] or payload [ %s ].", __FILE__, __LINE__, mqttRec.topic.c_str(), mqttRec.payLoad.c_str());
    }
}

void workProcessQueue() {
    mqttRecord_t mqttRec;
    
    while (!mqttQueue.isEmpty()) {
        if (shutter1.isActionInProgress() || shutter2.isActionInProgress()) {
            if (!suppressQueueLogMessage) {
                Log.notice("[ %s:%d ] MQTT message found in queue, but shutter action is still in progress. Wait for next cycle, available queue slots [ %d ]", __FILE__, __LINE__, mqttQueue.available());
            }
            suppressQueueLogMessage = true;
            break;
        }
        suppressQueueLogMessage = false;
        workMqttMessage(mqttQueue.shift());
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String strPayLoad = convertPayload(payload, length);

    Log.notice("[ %s:%d ] MQTT message arrived and enqueued with topic [ %s ] and payload [ %s ].", __FILE__, __LINE__, topic, strPayLoad.c_str());
    mqttQueue.push(mqttRecord_t{String(topic), strPayLoad});
}

bool connectToMqtt() {
    bool connected;

    char *user = NULL;
    char *pwd = NULL;

    if (strlen(mqttUser) > 0) {
        user = mqttUser;
    }
    if (strlen(mqttPassword) > 0) {
        pwd = mqttPassword;
    }

    Log.notice("[ %s:%d ] Connecting to MQTT broker [ %s:%s ] with client ID [ %s ].", __FILE__, __LINE__, mqttServer, mqttPort, clientId.c_str());
   
    connected = mqttClient.connect(clientId.c_str(), user, pwd, buildMqttTopic("availability", MqttMode::DEVICE).c_str(), 0, true, "offline", true);

    if (connected) {
        Log.notice("[ %s:%d ] Successfully connected to MQTT broker [ %s:%d ]", __FILE__, __LINE__, mqttServer, mqttPort);

        subscribeMqttTopic(buildMqttTopic("cmd", MqttMode::GLOBAL));

        subscribeMqttTopic(buildMqttTopic("set", MqttMode::SHUTTER1));
        subscribeMqttTopic(buildMqttTopic("set_position", MqttMode::SHUTTER1));

        subscribeMqttTopic(buildMqttTopic("set", MqttMode::SHUTTER2));
        subscribeMqttTopic(buildMqttTopic("set_position", MqttMode::SHUTTER2));

        announceMqtt();
    } else {
        Log.error("[ %s:%d ] Failed connection to MQTT broker [ %s:%d ] with status [ %d ].", __FILE__, __LINE__, mqttServer, mqttPort, mqttClient.state());
    }

    return connected;
}

void setupMqtt() {
    mqttClient.setServer(mqttServer, String(mqttPort).toInt());
    mqttClient.setCallback(mqttCallback);
}

bool checkMqttConnection() {
    bool isConnected = false;
    
    if (WiFi.isConnected()) {
        if (mqttClient.connected()) {
            stopBlinkOnboardLed();        
            mqttClient.loop();
            isConnected = true;
        } else {
            startBlinkOnboardLed();
            ulong now = millis();
            if (now - mqttLastReconnectAttempt > 5000) {
                mqttLastReconnectAttempt = now;
                if (connectToMqtt()) {
                    mqttLastReconnectAttempt = 0;
                }
            }
        }
    }

    return isConnected;
}

void shutterActionInProgress(String id, ShutterAction shutterAction) {

}

void shutterActionComplete(String id, ShutterAction shutterAction, ShutterReason reason) {
    if (shutter1.getID() = id) {
        sendStatusShutter1Mqtt();
    } else {
        sendStatusShutter2Mqtt();
    }
}

void setupShutter() {
    Log.notice("[ %s:%d ] Setup shutter 1", __FILE__, __LINE__);
    shutter1.setControlPins(D5, D6, D7);
    shutter1.setDurationFullMoveMs(15650);
    shutter1.setDelayTimeMs(String(shutterDelay).toInt());
    shutter1.onActionInProgress(shutterActionInProgress);
    shutter1.onActionComplete(shutterActionComplete);

    Log.notice("[ %s:%d ] Setup shutter 2", __FILE__, __LINE__);
    shutter2.setControlPins(D1, D2, D3);
    shutter2.setDurationFullMoveMs(15000);
    shutter2.setDelayTimeMs(String(shutterDelay).toInt());
    shutter2.onActionInProgress(shutterActionInProgress);
    shutter2.onActionComplete(shutterActionComplete);
}

void saveConfigCallback () {
    Log.trace("[ %s:%d ] WiFi manager registered changes, should save config", __FILE__, __LINE__);
    shouldSaveConfig = true;
}

void configModeCallback (WiFiManager *myWiFiManager) {
    Log.warning("Entered WiFi config mode with IP %s", WiFi.softAPIP().toString().c_str());
    Log.warning("SSID '%s'", myWiFiManager->getConfigPortalSSID().c_str());
  
    //entered config mode, make led toggle faster
    startBlinkOnboardLed(true);
}

void onWifiConnected (const WiFiEventStationModeConnected& event) {
    char bssid[20] = {0};
    sprintf(bssid,"%02X:%02X:%02X:%02X:%02X:%02X", event.bssid[0], event.bssid[1], event.bssid[2], event.bssid[3], event.bssid[4], event.bssid[5]);
    Log.notice("[ %s:%d ] Connected to SSID [ %s ] on BSSID [ %s ] via channel [ %d ], waiting for IP address.", __FILE__, __LINE__, event.ssid.c_str(), bssid, event.channel);
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
	Log.warning("[ %s:%d ] Disconnected from SSID [ %s ] with reason [ %d ].", __FILE__, __LINE__, event.ssid.c_str(), event.reason);
    startBlinkOnboardLed(true);
}

void onWifiGotIP(const WiFiEventStationModeGotIP& event) {
    Log.notice("[ %s:%d ] Received IP [ %s ] / Gateway [ %s ] / Mask [ %s ].", __FILE__, __LINE__, event.ip.toString().c_str(), event.gw.toString().c_str(), event.mask.toString().c_str());
	stopBlinkOnboardLed();

    MDNS.close();
    if (!MDNS.begin(clientId)) {
        Log.error("[ %s:%d ] Error setting up MDNS responder.", __FILE__, __LINE__);
    }
}

void setupWifiManager() {
    char charBuffer[1024];
    WiFiManager wifiManager;

    //uncomment to reset saved settings and clean file system for debug purpose
    //wifiManager.resetSettings(); LittleFS.format();

    // start blinking slow because we start in AP mode and try to connect
    startBlinkOnboardLed();

    clientId = String(CLIENT_ID_PREFIX) + String(ESP.getChipId());
    WiFi.hostname(clientId);
    WiFi.setAutoReconnect(true);    

    //read configuration from FS json
    Log.notice("[ %s:%d ] Mounting file system", __FILE__, __LINE__);

    if (LittleFS.begin()) {
        Log.notice("[ %s:%d ] Successfully mounted file system", __FILE__, __LINE__);
        if (LittleFS.exists("/config.json")) {
            //file exists, reading and loading
            Log.notice("[ %s:%d ] Reading JSON config file", __FILE__, __LINE__);
            File configFile = LittleFS.open("/config.json", "r");
            if (configFile) {
                Log.notice("[ %s:%d ] Successfully opened JSON config file", __FILE__, __LINE__);
                size_t size = configFile.size();
                // Allocate a buffer to store contents of the file.
                std::unique_ptr<char[]> buf(new char[size]);

                configFile.readBytes(buf.get(), size);

                DynamicJsonDocument json(1024);
                auto deserializeError = deserializeJson(json, buf.get());
                
                if ( ! deserializeError ) {
                    serializeJson(json, charBuffer, sizeof(charBuffer));
                    Log.notice("[ %s:%d ] Parsed JSON", __FILE__, __LINE__);
                    Log.notice("[ %s:%d ] %s", __FILE__, __LINE__, charBuffer);
                    
                    strcpy(mqttServer, json["mqtt_server"]);
                    strcpy(mqttPort, json["mqtt_port"]);
                    strcpy(mqttUser, json["mqtt_user"]);
                    strcpy(mqttPassword, json["mqtt_password"]);
                    strcpy(shutterDelay, json["shutter_delay"]);
                } else {
                    Log.error("[ %s:%d ] Failed to load JSON config file, consider reset", __FILE__, __LINE__);
                }
                
                configFile.close();
            }
        } else {
            Log.notice("[ %s:%d ] JSON config file does not exist", __FILE__, __LINE__);
        }
    } else {
        Log.fatal("[ %s:%d ] Failed to mount file system", __FILE__, __LINE__);
    }

    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    WiFiManagerParameter custom_mqtt_server("server", "MQTT broker", mqttServer, sizeof(mqttServer));
    wifiManager.addParameter(&custom_mqtt_server);

    WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqttPort, sizeof(mqttPort));
    wifiManager.addParameter(&custom_mqtt_port);

    WiFiManagerParameter custom_mqtt_user("user", "MQTT user", mqttUser, sizeof(mqttUser));
    wifiManager.addParameter(&custom_mqtt_user);

    WiFiManagerParameter custom_mqtt_password("password", "MQTT password", mqttPassword, sizeof(mqttPassword));
    wifiManager.addParameter(&custom_mqtt_password);

    WiFiManagerParameter custom_shutter_delay("delay", "Shutter delay in MS", shutterDelay, 4);
    wifiManager.addParameter(&custom_shutter_delay);

    wifiManager.setTimeout(120);
    if(!wifiManager.autoConnect()) {
        Log.fatal("[ %s:%d ] Failed to connect to WiFi and reached timeout, restart now...", __FILE__, __LINE__);
        delay(3000);
        
        //reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(5000);
    } 

    //if you get here you have connected to the WiFi
    Log.notice("[ %s:%d ] Connected to SSID [ %s ] on BSSID [ %s ] via channel [ %d ], waiting for IP address.", __FILE__, __LINE__, WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel());

    //read updated parameters
    strcpy(mqttServer, custom_mqtt_server.getValue());
    strcpy(mqttPort, custom_mqtt_port.getValue());
    strcpy(mqttUser, custom_mqtt_user.getValue());
    strcpy(mqttPassword, custom_mqtt_password.getValue());
    strcpy(shutterDelay, custom_shutter_delay.getValue());

    Log.notice("[ %s:%d ] MQTT broker settings [ %s:%s ]", __FILE__, __LINE__, mqttServer, mqttPort);
    Log.notice("[ %s:%d ] MQTT user [ %s ] and password  [ %s ]", __FILE__, __LINE__, mqttUser, mqttPassword);
    Log.notice("[ %s:%d ] Shutter delay [ %s ] ms", __FILE__, __LINE__, shutterDelay);

    //save the custom parameters to file system
    if (shouldSaveConfig) {
        Log.notice("[ %s:%d ] Saving JSON config", __FILE__, __LINE__);
        DynamicJsonDocument json(1024);
        json["mqtt_server"] = mqttServer;
        json["mqtt_port"] = mqttPort;
        json["mqtt_user"] = mqttUser;
        json["mqtt_password"] = mqttPassword;
        json["shutter_delay"] = shutterDelay;

        File configFile = LittleFS.open("/config.json", "w");
        if (!configFile) {
            Log.error("[ %s:%d ] Failed to open JSON config file for writing", __FILE__, __LINE__);
        }

        serializeJson(json, configFile);
        configFile.close();

        serializeJson(json, charBuffer, sizeof(charBuffer));
        Log.notice("[ %s:%d ] Successfully wrote config JSON", __FILE__, __LINE__);
        Log.notice(charBuffer);
    }

    stopBlinkOnboardLed();

    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
    wifiConnectHandler = WiFi.onStationModeConnected(onWifiConnected);
    wifiGotIpHandler = WiFi.onStationModeGotIP(onWifiGotIP);

    if (!MDNS.begin(clientId)) {
        Log.error("[ %s:%d ] Error setting up MDNS responder", __FILE__, __LINE__);
    }
}

void setup() {
    Serial.begin(115200);
    while(!Serial && !Serial.available()) {}
    Serial.println("\n");

    Log.begin(LOG_LEVEL_VERBOSE, &Serial, true);
    Log.setPrefix(printTimestamp);
    Log.setSuffix(printNewline);

    pinMode(LED_BUILTIN, OUTPUT);

    Log.notice("[ %s:%d ] Project version: %s", __FILE__, __LINE__, String(VERSION).c_str());
    Log.notice("[ %s:%d ] Build timestamp: %s", __FILE__, __LINE__, String(BUILD_TIMESTAMP).c_str());

    setupWifiManager();
    setupShutter();
    setupMqtt();    
}

void loop() {
    MDNS.update();
    checkMqttConnection();
    shutter1.tick();
    shutter2.tick();
    workProcessQueue();
}