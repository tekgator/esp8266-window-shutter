#include <config.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Ticker.h>
#include <PubSubClient.h>
#include <ArduinoLog.h>

const char* CLIENT_ID_PART1 = "esp8266";

String m_clientId;
WiFiEventHandler m_wifiConnectHandler;
WiFiEventHandler m_wifiDisconnectHandler;
WiFiEventHandler m_wifiGotIpHandler;
WiFiClient m_espClient;
Ticker m_wifiReconnectTimer;

PubSubClient m_mqttClient(m_espClient);
unsigned long m_lastReconnectAttempt = 0;

Ticker m_onboardLedBlinker;

enum WorkMode {
    INVALID_WORKMODE = -100,
    GLOBAL = -1,
    DEVICE = 0,
    COVER_LEFT = 1,
    COVER_RIGHT = 2,
};

enum CoverCommand {
    INVALID_COVERCOMMAND = -100,
    DOWN = 1,
    STOP = 2,
    UP = 3,
};

#define COVER_PINUP_LEFT D5
#define COVER_PINSTOP_LEFT D6
#define COVER_PINDOWN_LEFT D7
#define COVER_DURATION_LEFT 20
uint m_coverPositionLeft = 100;
unsigned long m_triggerStopPinLeft = 0; 

#define COVER_PINUP_RIGHT D1
#define COVER_PINSTOP_RIGHT D2
#define COVER_PINDOWN_RIGHT D3
#define COVER_DURATION_RIGHT 19
uint m_coverPositionRight = 100;
unsigned long m_triggerStopPinRight = 0;

bool m_triggerAnnounce = false;

void printTimestamp(Print* _logOutput) {
    char c[12];
    sprintf(c, "%10lu ", millis());
    _logOutput->print(c);
}

void printNewline(Print* _logOutput) {
  _logOutput->print('\n');
}


int roundUp(int numToRound, int multiple) {
    if (multiple == 0)
        return numToRound;

    int remainder = numToRound % multiple;
    if (remainder == 0)
        return numToRound;

    return numToRound + multiple - remainder;
}

uint getPin(WorkMode workMode, CoverCommand coverCommand) {
    uint pin = 0;

    if (workMode == WorkMode::COVER_LEFT) {
        if (coverCommand == CoverCommand::DOWN) {
            pin = COVER_PINDOWN_LEFT;
        } else if (coverCommand == CoverCommand::STOP) {
            pin = COVER_PINSTOP_LEFT;
        } else if (coverCommand == CoverCommand::UP) {
            pin = COVER_PINUP_LEFT;
        }    
    } else if (workMode == WorkMode::COVER_RIGHT) {
        if (coverCommand == CoverCommand::DOWN) {
            pin = COVER_PINDOWN_RIGHT;
        } else if (coverCommand == CoverCommand::STOP) {
            pin = COVER_PINSTOP_RIGHT;
        } else if (coverCommand == CoverCommand::UP) {
            pin = COVER_PINUP_RIGHT;
        }
    }

    return pin;
}

void checkStopPressRequired() {
    if (m_triggerStopPinLeft > 0 && millis() >= m_triggerStopPinLeft) {
        Log.notice("Press stop button on pin [ %d ] for cover [ %d / left ]", getPin(WorkMode::COVER_LEFT, CoverCommand::STOP), WorkMode::COVER_LEFT);
        
        digitalWrite(getPin(WorkMode::COVER_LEFT, CoverCommand::STOP), HIGH);
        delay(100);
        digitalWrite(getPin(WorkMode::COVER_LEFT, CoverCommand::STOP), LOW);

        m_triggerStopPinLeft = 0;
        m_triggerAnnounce = true;
    }

    if (m_triggerStopPinRight > 0 && millis() >= m_triggerStopPinRight) {
        Log.notice("Press stop button on pin [ %d ] for cover [ %d / right ]", getPin(WorkMode::COVER_RIGHT, CoverCommand::STOP), WorkMode::COVER_RIGHT);
        
        digitalWrite(getPin(WorkMode::COVER_RIGHT, CoverCommand::STOP), HIGH);
        delay(100);
        digitalWrite(getPin(WorkMode::COVER_RIGHT, CoverCommand::STOP), LOW);

        m_triggerStopPinRight = 0;
        m_triggerAnnounce = true;
    }
}

/*********  Blink onboard LED functions  **********/
void blinkOnboardLed() {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

void startBlinkOnboardLed(bool critical = false) {
    if (!m_onboardLedBlinker.active()) {
        m_onboardLedBlinker.attach_ms(critical ? 500 : 1000, blinkOnboardLed);
    }
}

void stopBlinkOnboardLed(bool leaveOn = true) {
    if (m_onboardLedBlinker.active()) {
        m_onboardLedBlinker.detach();
        digitalWrite(LED_BUILTIN, leaveOn ? LOW : HIGH);
    }
}


/*********  COVER functions  **********/

bool moveCoverByStatus(WorkMode workMode, CoverCommand coverCommand) {

    String strCoverCmd = "";
    String strCover = "";

    if (workMode == WorkMode::COVER_LEFT) {
        strCover = "left";

        if (m_triggerStopPinLeft > 0) {
            Log.warning("Cover [ %d / %s ] currently busy with other task, cannot proceed", workMode, strCover.c_str());
            return false;  
        }

        if (coverCommand == CoverCommand::DOWN) {
            strCoverCmd = "down";
            m_coverPositionLeft = 0;
        } else if (coverCommand == CoverCommand::STOP) {
            strCoverCmd = "stop";
        } else if (coverCommand == CoverCommand::UP) {
            strCoverCmd = "up";
            m_coverPositionLeft = 100;
        }
    } else {
        strCover = "right";

        if (m_triggerStopPinRight > 0) {
            Log.warning("Cover [ %d / %s ] currently busy with other task, cannot proceed", workMode, strCover.c_str());
            return false;  
        }

        if (coverCommand == CoverCommand::DOWN) {
            strCoverCmd = "down";
            m_coverPositionRight = 0;
        } else if (coverCommand == CoverCommand::STOP) {
            strCoverCmd = "stop";
        } else if (coverCommand == CoverCommand::UP) {
            strCoverCmd = "up";
            m_coverPositionRight = 100;
        }
    }

    Log.notice("Press button [ %d / %s ] / pin [ %d ] for cover [ %d / %s ]", coverCommand, strCoverCmd.c_str(), getPin(workMode, coverCommand), workMode, strCover.c_str());

    digitalWrite(getPin(workMode, coverCommand), HIGH);
    delay(100);
    digitalWrite(getPin(workMode, coverCommand), LOW);

    m_triggerAnnounce = true;

    return true;
}

bool moveCoverByPosition(WorkMode workMode, uint position) {
    String strCover = "";
    String strCoverCmd = "";
    
    CoverCommand coverCommand = CoverCommand::INVALID_COVERCOMMAND;

    int diffMovePercenct = 0;
    uint curPositionPercent = 0;
    uint newPositionPercent = 0;

    float timeToMoveComplete = 0;
    float timeToMove = 0;

    if (workMode == WorkMode::COVER_LEFT) {
        strCover = "left";
        
        if (m_triggerStopPinLeft > 0) {
            Log.warning("Cover [ %d / %s ] currently busy with other task, cannot proceed", workMode, strCover.c_str());
            return false;  
        }

        curPositionPercent = m_coverPositionLeft;
        timeToMoveComplete = COVER_DURATION_LEFT;
    } else {
        strCover = "right";

        if (m_triggerStopPinRight > 0) {
            Log.warning("Cover [ %d / %s ] currently busy with other task, cannot proceed", workMode, strCover.c_str());
            return false;  
        }

        curPositionPercent = m_coverPositionRight; 
        timeToMoveComplete = COVER_DURATION_RIGHT;
    }

    diffMovePercenct = curPositionPercent - position;

    if (diffMovePercenct > 0) {
        strCoverCmd = "down";
        coverCommand = CoverCommand::DOWN;
        newPositionPercent = curPositionPercent - roundUp(diffMovePercenct, 10);
        diffMovePercenct = curPositionPercent - newPositionPercent;
    } else if (diffMovePercenct < 0) {
        strCoverCmd = "up";
        coverCommand = CoverCommand::UP;
        newPositionPercent = curPositionPercent + roundUp(abs(diffMovePercenct), 10);
        diffMovePercenct = newPositionPercent - curPositionPercent;
    } else {
        // no difference detected, leave everything as is
        return true;
    }

    if (newPositionPercent <= 0 || newPositionPercent >= 100) {
        // full move without stop, use easy function
        return moveCoverByStatus(workMode, coverCommand);
    }

    timeToMove = (diffMovePercenct * timeToMoveComplete) / 100;

    Log.notice("Calculation for cover [ %d / %s ] done. Old pos [ %d ], new pos [ %d ], diff [ %d ], time to move [ %F ]", workMode, strCover.c_str(), curPositionPercent, newPositionPercent, diffMovePercenct, timeToMove);

    if (workMode == WorkMode::COVER_LEFT) {
        m_coverPositionLeft = newPositionPercent;
    } else {
        m_coverPositionRight = newPositionPercent;
    }

    Log.notice("Press button [ %d / %s ] / pin [ %d ] for cover [ %d / %s ]", coverCommand, strCoverCmd.c_str(), getPin(workMode, coverCommand), workMode, strCover.c_str());

    digitalWrite(getPin(workMode, coverCommand), HIGH);
    delay(100);
    digitalWrite(getPin(workMode, coverCommand), LOW);
    if (workMode == WorkMode::COVER_LEFT) {
        m_triggerStopPinLeft = millis() + (timeToMove * 1000);
    } else {
        m_triggerStopPinRight = millis() + (timeToMove * 1000);
    }

    return true;
}


/*********  MQTT functions  **********/

String buildMqttTopic (String subTopic, WorkMode workMode) {
    String mqttTopic;
    
    if (workMode == WorkMode::GLOBAL) {
        mqttTopic = String(CLIENT_ID_PART1) + "s/";
    } else {
        mqttTopic = m_clientId + "/";
    }

    if (workMode > WorkMode::DEVICE) {
        mqttTopic += "cover" + String(workMode) + "/";    
    }
    mqttTopic += subTopic;

    return mqttTopic;
}

void subscribeMqttTopic(String topic) {
    Log.notice("Subcribe to MQTT topic [ %s ].", topic.c_str());
    
    m_mqttClient.subscribe(topic.c_str());
}

void publishMqttTopic(String topic, String payload) {
    Log.notice("Publish MQTT topic [ %s ] with payload [ %s ].", topic.c_str(), payload.c_str());

    m_mqttClient.publish(topic.c_str(), payload.c_str());
}

WorkMode getWorkModeFromTopic(String topic) {
    WorkMode workMode = WorkMode::INVALID_WORKMODE;
    
    if (topic.startsWith(buildMqttTopic("", WorkMode::COVER_LEFT))) {
        workMode = WorkMode::COVER_LEFT;
    } else if (topic.startsWith(buildMqttTopic("", WorkMode::COVER_RIGHT))) {
        workMode = WorkMode::COVER_RIGHT;
    } else if (topic.startsWith(buildMqttTopic("", WorkMode::DEVICE))) {
        workMode = WorkMode::DEVICE;
    } else if (topic.startsWith(buildMqttTopic("", WorkMode::GLOBAL))) {
        workMode = WorkMode::GLOBAL;
    } 

    return workMode;
}

CoverCommand getCoverCommandFromPayload(String payload) {
    CoverCommand coverCommand = CoverCommand::INVALID_COVERCOMMAND;

    if (payload.equalsIgnoreCase("down")) {
        coverCommand = CoverCommand::DOWN;
    } else if (payload.equalsIgnoreCase("stop")) {
        coverCommand = CoverCommand::STOP;
    } else if (payload.equalsIgnoreCase("up")) {
        coverCommand = CoverCommand::UP;
    }

    return coverCommand;
}

void announceMqtt() {
    publishMqttTopic(buildMqttTopic("availability", WorkMode::DEVICE), "online");
    
    publishMqttTopic(buildMqttTopic("state", WorkMode::COVER_LEFT), m_coverPositionLeft == 0 ? "closed" : "open");
    publishMqttTopic(buildMqttTopic("position", WorkMode::COVER_LEFT), String(m_coverPositionLeft));

    publishMqttTopic(buildMqttTopic("state", WorkMode::COVER_RIGHT), m_coverPositionRight == 0 ? "closed" : "open");
    publishMqttTopic(buildMqttTopic("position", WorkMode::COVER_RIGHT), String(m_coverPositionRight));    
}

int getPosition(String payload) {
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

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String strTopic = topic;
    String strPayLoad = "";
    bool isValid = false;
    
    WorkMode workMode = WorkMode::INVALID_WORKMODE;
    CoverCommand coverCommand = CoverCommand::INVALID_COVERCOMMAND;
 
    for (uint i = 0; i < length; i++) {
        strPayLoad += (char)payload[i];
    }

    Log.notice("MQTT message arrived with topic [ %s ] and payload [ %s ].", strTopic.c_str(), strPayLoad.c_str());
    
    workMode = getWorkModeFromTopic(strTopic);

    if (workMode != WorkMode::INVALID_WORKMODE) {
        if (workMode == WorkMode::COVER_LEFT ||
            workMode == WorkMode::COVER_RIGHT) {
            if (strTopic.endsWith("set_position")) {
                if (getPosition(strPayLoad) >= 0) {
                    isValid = true;
                    moveCoverByPosition(workMode, getPosition(strPayLoad));
                }
            } else if (strTopic.endsWith("set")) {
                coverCommand = getCoverCommandFromPayload(strPayLoad);
                if (coverCommand != CoverCommand::INVALID_COVERCOMMAND) {
                    isValid = true;
                    moveCoverByStatus(workMode, coverCommand);
                }
            }
        } else if (workMode == WorkMode::GLOBAL) {
            if (strPayLoad.equalsIgnoreCase("announce")) {
                isValid = true;
                m_triggerAnnounce = true;
            }
        }
    }

    if (!isValid) {
        Log.warning("MQTT message cannot be processed, most likly incorrect topic [ %s ] or payload [ %s ].", strTopic.c_str(), strPayLoad.c_str());
    }
}

bool connectToMqtt() {
    bool connected;
    
    Log.notice("Connecting to MQTT broker [ %s:%d ] with client ID [ %s ].", MQTT_HOST.toString().c_str(), MQTT_PORT, m_clientId.c_str());
   
    connected = m_mqttClient.connect(m_clientId.c_str(), MQTT_USER, MQTT_PWD, buildMqttTopic("availability", WorkMode::DEVICE).c_str(), 0, true, "offline", true);

    if (connected) {
        Log.notice("Successfully connected to MQTT broker [ %s:%d ]", MQTT_HOST.toString().c_str(), MQTT_PORT);

        // WorkMode::GLOBAL
        subscribeMqttTopic(buildMqttTopic("command", WorkMode::GLOBAL));

        // WorkMode::DEVICE

        // WorkMode::COVER_LEFT
        subscribeMqttTopic(buildMqttTopic("set", WorkMode::COVER_LEFT));
        subscribeMqttTopic(buildMqttTopic("set_position", WorkMode::COVER_LEFT));

        // WorkMode::COVER_RIGHT
        subscribeMqttTopic(buildMqttTopic("set", WorkMode::COVER_RIGHT));
        subscribeMqttTopic(buildMqttTopic("set_position", WorkMode::COVER_RIGHT));

        m_triggerAnnounce = true;
    } else {
        Log.error("Failed connection to MQTT broker [ %s:%d ] with status [ %d ].", MQTT_HOST.toString().c_str(), MQTT_PORT, m_mqttClient.state());
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
            unsigned long now = millis();
            if (now - m_lastReconnectAttempt > 2000) {
                m_lastReconnectAttempt = now;
                if (connectToMqtt()) {
                    m_lastReconnectAttempt = 0;
                }
            }
        }
    }

    return isConnected;
}

/*********  WIFI functions  **********/
void connectToWifi() {
    Log.notice("Connecting to SSID [ %s ].", WIFI_SSID);
    startBlinkOnboardLed(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);   
}

void onWifiConnected (const WiFiEventStationModeConnected& event) {
    Log.notice("Connected to SSID [ %s ] on BSSID [ %02X:%02X:%02X:%02X:%02X:%02X ] via channel [ %d ], waiting for IP address.", event.ssid.c_str(), event.bssid[0], event.bssid[1], event.bssid[2], event.bssid[3], event.bssid[4], event.bssid[5], event.channel);
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
	Log.warning("Disconnected from SSID [ %s ] with reason [ %d ].", event.ssid.c_str(), event.reason);
    m_wifiReconnectTimer.once(2, connectToWifi);
}

void onWifiGotIP(const WiFiEventStationModeGotIP& event) {
    Log.notice("Received IP [ %s ] / Gateway [ %s ] / Mask [ %s ].", event.ip.toString().c_str(), event.gw.toString().c_str(), event.mask.toString().c_str());
	stopBlinkOnboardLed();

    if (!MDNS.begin(m_clientId)) {
        Log.error("Error setting up MDNS responder.");
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
    m_clientId = String(CLIENT_ID_PART1) + "-" + mac;

    WiFi.hostname(m_clientId);

    connectToWifi();    
}

/*********  Setup and Loop function  **********/
void setup() {
    Serial.begin(115200);
    while(!Serial && !Serial.available()){}
    Serial.println("\n");

    Log.begin(LOG_LEVEL_VERBOSE, &Serial, true);
    Log.setPrefix(printTimestamp);
    Log.setSuffix(printNewline);

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); // set to high will turn off internal LED

    // WorkMode::COVER_LEFT
    pinMode(getPin(WorkMode::COVER_LEFT, CoverCommand::UP), OUTPUT); // up
    pinMode(getPin(WorkMode::COVER_LEFT, CoverCommand::STOP), OUTPUT); // stop
    pinMode(getPin(WorkMode::COVER_LEFT, CoverCommand::DOWN), OUTPUT); // down
    
    //WorkMode::COVER_RIGHT
    pinMode(getPin(WorkMode::COVER_RIGHT, CoverCommand::UP), OUTPUT); // up
    pinMode(getPin(WorkMode::COVER_RIGHT, CoverCommand::STOP), OUTPUT); // stop
    pinMode(getPin(WorkMode::COVER_RIGHT, CoverCommand::DOWN), OUTPUT); // down

    setupMqtt();
    setupWifi();
}

void loop() {
    checkStopPressRequired();
    
    if (checkMqttConnection()) {
        if (m_triggerAnnounce) {
            announceMqtt();
            m_triggerAnnounce = false; 
        }
    }
}