#pragma once

#include <Arduino.h>
#include "Shutter/ShutterReason.hpp"
#include "Shutter/ShutterCallbacks.hpp"
#include "Shutter/ShutterAction.hpp"
#include "Shutter/ShutterTask.hpp"


class Shutter {

public:
    Shutter(String id);
    
    void onActionInProgress (ShutterInternals::OnActionInProgressUserCallback callback);
    void onActionComplete(ShutterInternals::OnActionCompleteUserCallback callback);

    String getID();
    void setID(String id);

    void setControlPins(uint pinUp, uint pinDown, uint pinStop);
    void setDurationFullMoveMs(uint ms);
    void setDelayTimeMs(uint ms);

    uint getPosition();
    bool setPosition(uint position, bool fOtherShutterActionInProgress = false);

    String getStatus();

    bool executeAction(ShutterAction shutterAction, uint position = 100, bool fOtherShutterActionInProgress = false);

    bool isActionInProgress();

    void tick();

private:
    String m_id;
    
    uint m_pinUp;
    uint m_pinDown;
    uint m_pinStop;

    uint m_delayTimeMs;
    uint m_durationFullMoveMs;
    uint m_lastButtonPressMs;

    uint m_position;
 
    ShutterInternals::ShutterTask m_task;

    int roundUp(int numToRound, int multiple);
    void setupPin(uint pin);
    uint getPin(ShutterAction shutterAction);
    uint getNewPosition(ShutterAction shutterAction);
    void resetTask();
    uint getDelayMs(bool fOtherShutterActionInProgress);

    std::vector<ShutterInternals::OnActionInProgressUserCallback> m_onActionInProgressUserCallbacks;
    std::vector<ShutterInternals::OnActionCompleteUserCallback> m_onActionCompleteUserCallbacks;

};

