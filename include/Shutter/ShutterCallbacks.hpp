#pragma once

#include <functional>
#include "ShutterReason.hpp"
#include "ShutterAction.hpp"

namespace ShutterInternals {

typedef std::function<void(String id, ShutterAction shutterAction)> OnActionInProgressUserCallback;
typedef std::function<void(String id, ShutterAction shutterAction, ShutterReason reason)> OnActionCompleteUserCallback;

}
