
#pragma once

#include "Shutter/ShutterAction.hpp"


namespace ShutterInternals {

typedef struct {
    ulong executionTimeMillis = 0;
    ShutterAction shutterAction = ShutterAction::UNDEFINED_ACTION;
    uint stopRequiredAfterMillis = 0;
    uint newPosition = 0;
    bool reportProgressBegin = true;
} ShutterTask;

}