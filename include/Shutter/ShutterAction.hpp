#pragma once

enum class ShutterAction : int8_t {
    UNDEFINED_ACTION = -100,
    MOVE_BY_POSITION,
    UP = 1,
    DOWN,
    STOP,
};