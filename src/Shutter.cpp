#include <ArduinoLog.h>
#include "Shutter.hpp"

Shutter::Shutter(String id) : 
    m_pinUp(0),
    m_pinDown(0),
    m_pinStop(0),
    m_durationFullMoveMs(20000),
    m_lastButtonPressMs(0),
    m_position(100) {
    m_id = id;
    resetTask();
}

int Shutter::roundUp(int numToRound, int multiple) {
    if (multiple == 0)
        return numToRound;

    int remainder = numToRound % multiple;
    if (remainder == 0)
        return numToRound;

    return numToRound + multiple - remainder;
}

String Shutter::getID() {
    return m_id;
}

void Shutter::setupPin(uint pin) {
    pinMode(pin, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
}

void Shutter::setControlPins(uint pinUp, uint pinDown, uint pinStop) {
    m_pinUp = pinUp;
    setupPin(m_pinUp);

    m_pinDown = pinDown;
    setupPin(m_pinDown);

    m_pinStop = pinStop;
    setupPin(m_pinStop);

    Log.notice("[ %s:%d ] [ %s ] Pin setup complete with up [ %d ], down [ %d ], stop [ %d ].", __FILE__, __LINE__, m_id.c_str(), m_pinUp, m_pinDown, m_pinStop);
}

void Shutter::setDurationFullMoveMs(uint ms) {
    m_durationFullMoveMs = ms;
    Log.notice("[ %s:%d ] [ %s ] Received duration for full shutter move [ %dms ].", __FILE__, __LINE__, m_id.c_str(), m_durationFullMoveMs);
}

void Shutter::setDelayTimeMs(uint ms) {
    m_delayTimeMs = ms;
    Log.notice("[ %s:%d ] [ %s ] Received delay time required before next action can be executed [ %dms ].", __FILE__, __LINE__, m_id.c_str(), m_delayTimeMs);
}

uint Shutter::getPin(ShutterAction shutterAction) {
    uint pin = 0;

    switch (shutterAction) {
        case ShutterAction::UP:
            pin = m_pinUp;
            break;
    
        case ShutterAction::DOWN:
            pin = m_pinDown;
            break;

        case ShutterAction::STOP:
            pin = m_pinStop;
            break;

        default:
            break;
    }

    return pin;
}

uint Shutter::getNewPosition(ShutterAction shutterAction) {
    uint position;

    switch (shutterAction) {
        case ShutterAction::UP:
            position = 100;
            break;
    
        case ShutterAction::DOWN:
            position = 0;
            break;

        default:
            position = m_position;
            break;
    }

    return position;
}

void Shutter::resetTask() {
    m_task.executionTimeMillis = 0;
    m_task.newPosition = 0;
    m_task.shutterAction = ShutterAction::UNDEFINED_ACTION;
    m_task.stopRequiredAfterMillis = 0;
    m_task.reportProgressBegin = true;
}

uint Shutter::getPosition() {
    return m_position;
}

bool Shutter::setPosition(uint position) {
    bool success = true;

    int diffMovePercenct;
    uint newPositionPercent;
    ShutterAction shutterAction;
    
    position = min((int) position, 100);
    diffMovePercenct = m_position - position;

    if (diffMovePercenct > 0) {
        shutterAction = ShutterAction::DOWN;
        newPositionPercent = m_position - roundUp(diffMovePercenct, 10);
        diffMovePercenct = m_position - newPositionPercent;
    } else if (diffMovePercenct < 0) {
        shutterAction = ShutterAction::UP;
        newPositionPercent = m_position + roundUp(abs(diffMovePercenct), 10);
        diffMovePercenct = newPositionPercent - m_position;
    } else {
        // no difference detected, leave everything as is
        for (auto callback : m_onActionCompleteUserCallbacks) {
            callback(m_id, ShutterAction::MOVE_BY_POSITION, ShutterReason::SUCCESS);
        }
        return success;
    }

    if (newPositionPercent <= 0 || newPositionPercent >= 100) {
        // full move (to the end) without stop, use executeAction function
        return executeAction(shutterAction);
    }

    resetTask();
    m_task.executionTimeMillis = millis();
    m_task.newPosition = newPositionPercent;
    m_task.shutterAction = shutterAction;
    m_task.stopRequiredAfterMillis = (abs(diffMovePercenct) * m_durationFullMoveMs) / 100;

    Log.notice("[ %s:%d ] [ %s ] Calculation for new position completed. Action [ %d ], Old pos [ %d ], new pos [ %d ], diff [ %d ], time to move [ %dms ].", __FILE__, __LINE__, m_id.c_str(), m_task.shutterAction, m_position, m_task.newPosition, diffMovePercenct, m_task.stopRequiredAfterMillis);        

    return success;
}

String Shutter::getStatus() {
    return m_position == 0 ? "closed" : "open";
}

bool Shutter::isActionInProgress() {
    return (m_task.executionTimeMillis > 0) || 
           (millis() - m_lastButtonPressMs < m_delayTimeMs);
}

bool Shutter::executeAction(ShutterAction shutterAction, uint position) {
    bool success = true;

    if (isActionInProgress()) {
        Log.warning("[ %s:%d ] [ %s ] Device currently busy with other task, cannot proceed with action [ %d ].", __FILE__, __LINE__, m_id.c_str(), shutterAction);
        
        success = false;
        for (auto callback : m_onActionCompleteUserCallbacks) {
            callback(m_id, shutterAction, ShutterReason::DEVICE_BUSY);
        }
    } else {
        if (shutterAction == ShutterAction::MOVE_BY_POSITION) {
            setPosition(position);
        } else {
            resetTask();
            m_task.executionTimeMillis = millis();
            m_task.shutterAction = shutterAction;
            m_task.newPosition = getNewPosition(m_task.shutterAction);
            
            Log.notice("[ %s:%d ] [ %s ] Scheduled task with action [ %d ], new position [ %d ], no STOP required.", __FILE__, __LINE__, m_id.c_str(), m_task.shutterAction, m_task.newPosition);
        }
    }

    return success;
}

void Shutter::tick() {
    if (m_task.executionTimeMillis > 0 && 
        (long) millis() - (long) m_task.executionTimeMillis >= 0) {
        Log.notice("[ %s:%d ] [ %s ] Execute scheduled task with action [ %d ], new position [ %d ], report progress begin [ %T ].", __FILE__, __LINE__, m_id.c_str(), m_task.shutterAction, m_task.newPosition, m_task.reportProgressBegin);
        
        if (m_task.reportProgressBegin) {
            for (auto callback : m_onActionInProgressUserCallbacks) {
                callback(m_id, m_task.shutterAction);
            }
        }

        uint pin = getPin(m_task.shutterAction);
        digitalWrite(pin, HIGH);
        delay(100);
        digitalWrite(pin, LOW);
        m_lastButtonPressMs = millis();

        if (m_task.stopRequiredAfterMillis > 0) {
            m_task.shutterAction = ShutterAction::STOP;
            m_task.executionTimeMillis = millis() + m_task.stopRequiredAfterMillis;
            m_task.stopRequiredAfterMillis = 0;
            m_task.reportProgressBegin = false;
            //m_task.newPosition = remain untouched as it is set in the next loop

            Log.notice("[ %s:%d ] [ %s ] Schedule required STOP task in [ %dms ] to reach new position [ %d ].", __FILE__, __LINE__, m_id.c_str(), m_task.executionTimeMillis, m_task.newPosition);
        } else {
            Log.notice("[ %s:%d ] [ %s ] Scheduled task finished for action [ %d ], new position [ %d ].", __FILE__, __LINE__, m_id.c_str(), m_task.shutterAction, m_task.newPosition);
            
            m_position = m_task.newPosition;
            resetTask();
            for (auto callback : m_onActionCompleteUserCallbacks) {
                callback(m_id, m_task.shutterAction, ShutterReason::SUCCESS);
            }
        }
    }
}

void Shutter::onActionInProgress(ShutterInternals::OnActionInProgressUserCallback callback) {
    m_onActionInProgressUserCallbacks.push_back(callback);
}

void Shutter::onActionComplete(ShutterInternals::OnActionCompleteUserCallback callback) {
    m_onActionCompleteUserCallbacks.push_back(callback);
}