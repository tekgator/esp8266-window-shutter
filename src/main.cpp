#include <Arduino.h>
#include <ArduinoLog.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Ticker.h>
#include <PubSubClient.h>
#include "config.h"
#include "Shutter.hpp"

Shutter m_shutter1("left");
Shutter m_shutter2("right");

WiFiEventHandler m_wifiConnectHandler;
WiFiEventHandler m_wifiDisconnectHandler;
WiFiEventHandler m_wifiGotIpHandler;
String m_clientId;
WiFiClient m_wifiClient;
Ticker m_wifiReconnectTimer;

PubSubClient m_mqttClient(m_wifiClient);
unsigned long m_mqttLastReconnectAttempt = 0;

enum MqttMode {
    INVALID_MQTT_MODE = -100,
    GLOBAL = -1,
    DEVICE = 0,
    SHUTTER1 = 1,
    SHUTTER2 = 2,
};

Ticker m_onboardLedBlinker;

void blinkOnboardLed() {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

void startBlinkOnboardLed(bool critical = false) {
    if (!m_onboardLedBlinker.active()) {
        m_onboardLedBlinker.attach_ms(critical ? 500 : 1000, blinkOnboardLed);
    }
}

void stopBlinkOnboardLed() {
    if (m_onboardLedBlinker.active()) {
        m_onboardLedBlinker.detach();
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
        mqttTopic = m_clientId + "/";
    }

    if (mqttMode > MqttMode::DEVICE) {
        mqttTopic += "shutter" + String(mqttMode) + "/";    
    }
    mqttTopic += subTopic;

    return mqttTopic;
}

void subscribeMqttTopic(String topic) {
    Log.notice("[ %s:%d ] Subcribe to MQTT topic [ %s ].", __FILE__, __LINE__, topic.c_str());
    m_mqttClient.subscribe(topic.c_str());
}

void publishMqttTopic(String topic, String payload) {
    Log.notice("[ %s:%d ] Publish MQTT topic [ %s ] with payload [ %s ].", __FILE__, __LINE__, topic.c_str(), payload.c_str());
    m_mqttClient.publish(topic.c_str(), payload.c_str());
}

void sendStatusShutter1Mqtt() {
    publishMqttTopic(buildMqttTopic("state", MqttMode::SHUTTER1), m_shutter1.getStatus());
    publishMqttTopic(buildMqttTopic("position", MqttMode::SHUTTER1), String(m_shutter1.getPosition()));
}

void sendStatusShutter2Mqtt() {
    publishMqttTopic(buildMqttTopic("state", MqttMode::SHUTTER2), m_shutter2.getStatus());
    publishMqttTopic(buildMqttTopic("position", MqttMode::SHUTTER2), String(m_shutter2.getPosition()));
}

void announceMqtt() {
    publishMqttTopic(buildMqttTopic("availability", MqttMode::DEVICE), "online");
        
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

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String strTopic = topic;
    String strPayLoad = convertPayload(payload, length);
    MqttMode mqttMode = getMqttModeFromTopic(strTopic);
    bool isValid = false;
    int position = -1;
    ShutterAction shutterAction = ShutterAction::UNDEFINED_ACTION;

    Log.notice("[ %s:%d ] MQTT message arrived with topic [ %s ] and payload [ %s ].", __FILE__, __LINE__, strTopic.c_str(), strPayLoad.c_str());

    if (mqttMode != MqttMode::INVALID_MQTT_MODE) {
        if (mqttMode == MqttMode::SHUTTER1 ||
            mqttMode == MqttMode::SHUTTER2) {
            if (strTopic.endsWith("set_position")) {
                position = getPositionFromPayload(strPayLoad);
                if (position >= 0) {
                    shutterAction = ShutterAction::MOVE_BY_POSITION;
                    isValid = true;
                }
            } else if (strTopic.endsWith("set")) {
                shutterAction = getShutterActionFromPayload(strPayLoad);
                if (shutterAction != ShutterAction::UNDEFINED_ACTION) {
                    isValid = true;
                }
            }

            if (isValid) {
                if (mqttMode == MqttMode::SHUTTER1) {
                    m_shutter1.executeAction(shutterAction, position, m_shutter2.isActionInProgress());
                } else if (mqttMode == MqttMode::SHUTTER2) {
                    m_shutter2.executeAction(shutterAction, position, m_shutter1.isActionInProgress());
                }                
            }
        } else if (mqttMode == MqttMode::GLOBAL) {
            if (strPayLoad == "announce") {
                isValid = true;
                announceMqtt();
             }
        }
    }

    if (!isValid) {
        Log.warning("[ %s:%d ] MQTT message cannot be processed, most likly incorrect topic [ %s ] or payload [ %s ].", __FILE__, __LINE__, strTopic.c_str(), strPayLoad.c_str());
    }

}

bool connectToMqtt() {
    bool connected;
    
    Log.notice("[ %s:%d ] Connecting to MQTT broker [ %s:%d ] with client ID [ %s ].", __FILE__, __LINE__, MQTT_HOST.toString().c_str(), MQTT_PORT, m_clientId.c_str());
   
    connected = m_mqttClient.connect(m_clientId.c_str(), MQTT_USER, MQTT_PWD, buildMqttTopic("availability", MqttMode::DEVICE).c_str(), 0, true, "offline", true);

    if (connected) {
        Log.notice("[ %s:%d ] Successfully connected to MQTT broker [ %s:%d ]", __FILE__, __LINE__, MQTT_HOST.toString().c_str(), MQTT_PORT);

        subscribeMqttTopic(buildMqttTopic("command", MqttMode::GLOBAL));

        subscribeMqttTopic(buildMqttTopic("set", MqttMode::SHUTTER1));
        subscribeMqttTopic(buildMqttTopic("set_position", MqttMode::SHUTTER1));

        subscribeMqttTopic(buildMqttTopic("set", MqttMode::SHUTTER2));
        subscribeMqttTopic(buildMqttTopic("set_position", MqttMode::SHUTTER2));

        announceMqtt();
    } else {
        Log.error("[ %s:%d ] Failed connection to MQTT broker [ %s:%d ] with status [ %d ].", __FILE__, __LINE__, MQTT_HOST.toString().c_str(), MQTT_PORT, m_mqttClient.state());
    }

    return connected;
}

void setupMqtt() {
    m_mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    m_mqttClient.setCallback(mqttCallback);
}

bool checkMqttConnection() {
    bool isConnected = false;
    
    if (WiFi.isConnected()) {
        if (m_mqttClient.connected()) {
            stopBlinkOnboardLed();        
            m_mqttClient.loop();
            isConnected = true;
        } else {
            startBlinkOnboardLed();
            ulong now = millis();
            if (now - m_mqttLastReconnectAttempt > 2000) {
                m_mqttLastReconnectAttempt = now;
                if (connectToMqtt()) {
                    m_mqttLastReconnectAttempt = 0;
                }
            }
        }
    }

    return isConnected;
}

void shutterActionInProgress(String id, ShutterAction shutterAction) {

}

void shutterActionComplete(String id, ShutterAction shutterAction, ShutterReason reason) {
    if (m_shutter1.getID() = id) {
        sendStatusShutter1Mqtt();
    } else {
        sendStatusShutter2Mqtt();
    }
}

void setupShutter() {
    Log.notice("[ %s:%d ] Setup shutter 1.", __FILE__, __LINE__);
    m_shutter1.setControlPins(D5, D6, D7);
    m_shutter1.setDurationFullMoveMs(15650);
    m_shutter1.onActionInProgress(shutterActionInProgress);
    m_shutter1.onActionComplete(shutterActionComplete);

    Log.notice("[ %s:%d ] Setup shutter 2.", __FILE__, __LINE__);
    m_shutter2.setControlPins(D1, D2, D3);
    m_shutter2.setDurationFullMoveMs(15000);
    m_shutter2.onActionInProgress(shutterActionInProgress);
    m_shutter2.onActionComplete(shutterActionComplete);
}

void connectToWifi() {
    Log.notice("[ %s:%d ] Connecting to SSID [ %s ].", __FILE__, __LINE__, WIFI_SSID);
    startBlinkOnboardLed(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);   
}

void onWifiConnected (const WiFiEventStationModeConnected& event) {
    char bssid[20] = {0};
    sprintf(bssid,"%02X:%02X:%02X:%02X:%02X:%02X", event.bssid[0], event.bssid[1], event.bssid[2], event.bssid[3], event.bssid[4], event.bssid[5]);
    Log.notice("[ %s:%d ] Connected to SSID [ %s ] on BSSID [ %s ] via channel [ %d ], waiting for IP address.", __FILE__, __LINE__, event.ssid.c_str(), bssid, event.channel);
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
	Log.warning("[ %s:%d ] Disconnected from SSID [ %s ] with reason [ %d ].", __FILE__, __LINE__, event.ssid.c_str(), event.reason);
    m_wifiReconnectTimer.once(2, connectToWifi);
}

void onWifiGotIP(const WiFiEventStationModeGotIP& event) {
    Log.notice("[ %s:%d ] Received IP [ %s ] / Gateway [ %s ] / Mask [ %s ].", __FILE__, __LINE__, event.ip.toString().c_str(), event.gw.toString().c_str(), event.mask.toString().c_str());
	stopBlinkOnboardLed();

    if (!MDNS.begin(m_clientId)) {
        Log.error("[ %s:%d ] Error setting up MDNS responder.", __FILE__, __LINE__);
    }
}

void setupWifi() {
    WiFi.disconnect() ;
    WiFi.persistent(false);
    m_wifiConnectHandler = WiFi.onStationModeConnected(onWifiConnected);
    m_wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
    m_wifiGotIpHandler = WiFi.onStationModeGotIP(onWifiGotIP);

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    m_clientId = String(CLIENT_ID_PREFIX) + "-" + mac;

    WiFi.hostname(m_clientId);

    connectToWifi();    
}

void setup() {
    Serial.begin(115200);
    while(!Serial && !Serial.available()) {}
    Serial.println("\n");

    Log.begin(LOG_LEVEL_VERBOSE, &Serial, true);
    Log.setPrefix(printTimestamp);
    Log.setSuffix(printNewline);

    pinMode(LED_BUILTIN, OUTPUT);

    setupShutter();
    setupMqtt();    
    setupWifi();
}

void loop() {
    checkMqttConnection();
    m_shutter1.tick();
    m_shutter2.tick();
}