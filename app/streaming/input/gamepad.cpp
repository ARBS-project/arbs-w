#include "streaming/session.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "settings/mappingmanager.h"
#include "utils.h"

#include <QtMath>

#ifdef ARBS_W
#include <cstdio>
#include "arbs/arbs_bridge.h"
#include "arbs/arbs_numbering.h"
#endif

// How long the Start button must be pressed to toggle mouse emulation
#define MOUSE_EMULATION_LONG_PRESS_TIME 750

// How long between polling the gamepad to send virtual mouse input
#define MOUSE_EMULATION_POLLING_INTERVAL 50

// Determines how fast the mouse will move each interval
#define MOUSE_EMULATION_MOTION_MULTIPLIER 4

// Determines the maximum motion amount before allowing movement
#define MOUSE_EMULATION_DEADZONE 2

// Haptic capabilities (in addition to those from SDL_HapticQuery())
#define ML_HAPTIC_GC_RUMBLE         (1U << 16)
#define ML_HAPTIC_SIMPLE_RUMBLE     (1U << 17)
#define ML_HAPTIC_GC_TRIGGER_RUMBLE (1U << 18)

const int SdlInputHandler::k_ButtonMap[] = {
    A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
    BACK_FLAG, SPECIAL_FLAG, PLAY_FLAG,
    LS_CLK_FLAG, RS_CLK_FLAG,
    LB_FLAG, RB_FLAG,
    UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
    MISC_FLAG,
    PADDLE1_FLAG, PADDLE2_FLAG, PADDLE3_FLAG, PADDLE4_FLAG,
    TOUCHPAD_FLAG,
};

GamepadState*
SdlInputHandler::findStateForGamepad(SDL_JoystickID id)
{
    int i;

    for (i = 0; i < MAX_GAMEPADS; i++) {
        if (m_GamepadState[i].jsId == id) {
            SDL_assert(!m_MultiController || m_GamepadState[i].index == i);
            return &m_GamepadState[i];
        }
    }

    // We can get a spurious removal event if the device is removed
    // before or during SDL_GameControllerOpen(). This is fine to ignore.
    return nullptr;
}

void SdlInputHandler::sendGamepadState(GamepadState* state)
{
    SDL_assert(m_GamepadMask == 0x1 || m_MultiController);

#ifdef ARBS_W
    if (state->arbsHidden) {
        // ARBS-W B2c 2-1: APEX-only mode - a hidden pad sends nothing to the host (local state is kept)
        return;
    }
#endif

    // Handle Select+PS as the clickpad button on PS4/5 controllers without a clickpad mapping
    int buttons = state->buttons;
    if (state->clickpadButtonEmulationEnabled) {
        if (state->buttons == (BACK_FLAG | SPECIAL_FLAG)) {
            buttons = MISC_FLAG;
            state->emulatedClickpadButtonDown = true;
        }
        else if (state->emulatedClickpadButtonDown) {
            buttons &= ~MISC_FLAG;
            state->emulatedClickpadButtonDown = false;
        }
    }

    unsigned char lt = state->lt;
    unsigned char rt = state->rt;
    short lsX = state->lsX;
    short lsY = state->lsY;
    short rsX = state->rsX;
    short rsY = state->rsY;

    // When in single controller mode, merge all gamepad state together
    if (!m_MultiController) {
        for (int i = 0; i < MAX_GAMEPADS; i++) {
#ifdef ARBS_W
            if (m_GamepadState[i].arbsHidden) {
                // ARBS-W B2c 2-1: a hidden pad's input is not merged into number 0 either
                continue;
            }
#endif
            if (m_GamepadState[i].index == state->index) {
                buttons |= m_GamepadState[i].buttons;
                if (lt < m_GamepadState[i].lt) {
                    lt = m_GamepadState[i].lt;
                }
                if (rt < m_GamepadState[i].rt) {
                    rt = m_GamepadState[i].rt;
                }

                // We use abs() here instead of qAbs() for get proper integer promotion to
                // correctly handle abs(-32768), which is not representable in a short.
                if (abs(lsX) < abs(m_GamepadState[i].lsX) || abs(lsY) < abs(m_GamepadState[i].lsY)) {
                    lsX = m_GamepadState[i].lsX;
                    lsY = m_GamepadState[i].lsY;
                }
                if (abs(rsX) < abs(m_GamepadState[i].rsX) || abs(rsY) < abs(m_GamepadState[i].rsY)) {
                    rsX = m_GamepadState[i].rsX;
                    rsY = m_GamepadState[i].rsY;
                }
            }
        }
    }

    LiSendMultiControllerEvent(state->index,
                               m_GamepadMask,
                               buttons,
                               lt,
                               rt,
                               lsX,
                               lsY,
                               rsX,
                               rsY);
}

void SdlInputHandler::sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level)
{
    uint8_t batteryPercentage;
    uint8_t batteryState;

    // SDL's battery reporting capabilities are quite limited. Notably, we cannot
    // tell the battery level while charging (or even if a battery is present).
    // We also cannot tell the percentage of charge exactly in any case.
    switch (level)
    {
    case SDL_JOYSTICK_POWER_UNKNOWN:
        batteryState = LI_BATTERY_STATE_UNKNOWN;
        batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        break;
    case SDL_JOYSTICK_POWER_WIRED:
        batteryState = LI_BATTERY_STATE_CHARGING;
        batteryPercentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        break;
    case SDL_JOYSTICK_POWER_EMPTY:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 5;
        break;
    case SDL_JOYSTICK_POWER_LOW:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 20;
        break;
    case SDL_JOYSTICK_POWER_MEDIUM:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 50;
        break;
    case SDL_JOYSTICK_POWER_FULL:
        batteryState = LI_BATTERY_STATE_DISCHARGING;
        batteryPercentage = 90;
        break;
    default:
        return;
    }

    LiSendControllerBatteryEvent(state->index, batteryState, batteryPercentage);
}

Uint32 SdlInputHandler::mouseEmulationTimerCallback(Uint32 interval, void *param)
{
    auto gamepad = reinterpret_cast<GamepadState*>(param);

    int rawX;
    int rawY;

    // Determine which analog stick is currently receiving the strongest input
    if (abs(gamepad->lsX) + abs(gamepad->lsY) > abs(gamepad->rsX) + abs(gamepad->rsY)) {
        rawX = gamepad->lsX;
        rawY = -gamepad->lsY;
    }
    else {
        rawX = gamepad->rsX;
        rawY = -gamepad->rsY;
    }

    float deltaX;
    float deltaY;

    // Produce a base vector for mouse movement with increased speed as we deviate further from center
    deltaX = qPow(rawX / 32766.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);
    deltaY = qPow(rawY / 32766.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);

    // Enforce deadzones
    deltaX = qAbs(deltaX) > MOUSE_EMULATION_DEADZONE ? deltaX - MOUSE_EMULATION_DEADZONE : 0;
    deltaY = qAbs(deltaY) > MOUSE_EMULATION_DEADZONE ? deltaY - MOUSE_EMULATION_DEADZONE : 0;

    if (deltaX != 0 || deltaY != 0) {
        LiSendMouseMoveEvent((short)deltaX, (short)deltaY);
    }

    return interval;
}

static void updateAnalogStickAxis(short& value, short newValue, bool& dirty)
{
#ifdef STEAM_LINK
    // Use a deadzone on Steam Link to reduce CPU usage from idle joysticks
    newValue = abs(newValue) < 1500 ? 0 : newValue;
#endif
    dirty |= (newValue != value);
    value = newValue;
}

static void updateTriggerAxis(unsigned char& value, unsigned char newValue, bool& dirty)
{
#ifdef STEAM_LINK
    // Use a deadzone on Steam Link to reduce CPU usage from idle joysticks
    newValue = newValue < 10 ? 0 : newValue;
#endif
    dirty |= (newValue != value);
    value = newValue;
}

void SdlInputHandler::handleControllerAxisEvent(SDL_ControllerAxisEvent* event)
{
    SDL_JoystickID gameControllerId = event->which;
    GamepadState* state = findStateForGamepad(gameControllerId);
    if (state == NULL) {
        return;
    }

#ifdef ARBS_W
    // ARBS-W: APEX input seen (GetInfo readiness) + trigger positions for the trigger effects
    const bool arbsApex = ArbsBridge::isApexJoystick(gameControllerId);
    if (arbsApex) {
        ArbsBridge::onApexInput();
    }
#endif

    // Batch all pending axis motion events for this gamepad to save CPU time
    SDL_Event nextEvent;
    bool dirty = false;
    for (;;) {
#ifdef ARBS_W
        if (arbsApex && (event->axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || event->axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            ArbsBridge::onApexTriggerAxis(event->axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT, event->value);
        }
#endif
        switch (event->axis)
        {
            case SDL_CONTROLLER_AXIS_LEFTX:
                updateAnalogStickAxis(state->lsX, event->value, dirty);
                break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                // Signed values have one more negative value than
                // positive value, so inverting the sign on -32768
                // could actually cause the value to overflow and
                // wrap around to be negative again. Avoid that by
                // capping the value at 32767.
                updateAnalogStickAxis(state->lsY, -qMax(event->value, (short)-32767), dirty);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
                updateAnalogStickAxis(state->rsX, event->value, dirty);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
                updateAnalogStickAxis(state->rsY, -qMax(event->value, (short)-32767), dirty);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                updateTriggerAxis(state->lt, (unsigned char)(event->value * 255UL / 32767), dirty);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                updateTriggerAxis(state->rt, (unsigned char)(event->value * 255UL / 32767), dirty);
                break;
            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Unhandled controller axis: %d",
                            event->axis);
                return;
        }

        // Check for another event to batch with
        if (SDL_PeepEvents(&nextEvent, 1, SDL_PEEKEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERAXISMOTION) <= 0) {
            break;
        }

        event = &nextEvent.caxis;
        if (event->which != gameControllerId) {
            // Stop batching if a different gamepad interrupts us
            break;
        }

        // Remove the next event to batch
        SDL_PeepEvents(&nextEvent, 1, SDL_GETEVENT, SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERAXISMOTION);
    }

    // Only send the gamepad state to the host if it's not in mouse emulation mode
    if (state->mouseEmulationTimer == 0 && dirty) {
        sendGamepadState(state);
    }
}

void SdlInputHandler::handleControllerButtonEvent(SDL_ControllerButtonEvent* event)
{
    if (event->button >= SDL_arraysize(k_ButtonMap)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No mapping for gamepad button: %u",
                    event->button);
        return;
    }

    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }

#ifdef ARBS_W
    if (ArbsBridge::isApexJoystick(event->which)) {
        ArbsBridge::onApexInput();
    }
    // ARBS-W B2c provisional decision D2 (planner has not ruled): a hidden pad never starts mouse emulation, because its
    // mouse/keyboard events would reach the host as input. The quit combo stays usable (local SDL_QUIT only).
    const bool gamepadMouse = m_GamepadMouse && !state->arbsHidden;
#else
    const bool gamepadMouse = m_GamepadMouse;
#endif

    if (m_SwapFaceButtons) {
        switch (event->button) {
        case SDL_CONTROLLER_BUTTON_A:
            event->button = SDL_CONTROLLER_BUTTON_B;
            break;
        case SDL_CONTROLLER_BUTTON_B:
            event->button = SDL_CONTROLLER_BUTTON_A;
            break;
        case SDL_CONTROLLER_BUTTON_X:
            event->button = SDL_CONTROLLER_BUTTON_Y;
            break;
        case SDL_CONTROLLER_BUTTON_Y:
            event->button = SDL_CONTROLLER_BUTTON_X;
            break;
        }
    }

    if (event->state == SDL_PRESSED) {
        state->buttons |= k_ButtonMap[event->button];

        if (event->button == SDL_CONTROLLER_BUTTON_START) {
            state->lastStartDownTime = SDL_GetTicks();
        }
        else if (state->mouseEmulationTimer != 0) {
            if (event->button == SDL_CONTROLLER_BUTTON_A) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_B) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_X) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X2);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                LiSendScrollEvent(1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                LiSendScrollEvent(-1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                LiSendHScrollEvent(1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                LiSendHScrollEvent(-1);
            }
        }
    }
    else {
        state->buttons &= ~k_ButtonMap[event->button];

        if (event->button == SDL_CONTROLLER_BUTTON_START) {
            if (SDL_GetTicks() - state->lastStartDownTime > MOUSE_EMULATION_LONG_PRESS_TIME) {
                if (state->mouseEmulationTimer != 0) {
                    SDL_RemoveTimer(state->mouseEmulationTimer);
                    state->mouseEmulationTimer = 0;

                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Mouse emulation deactivated");
                    Session::get()->notifyMouseEmulationMode(false);
                }
                else if (gamepadMouse) {
                    // Send the start button up event to the host, since we won't do it below
                    sendGamepadState(state);

                    state->mouseEmulationTimer = SDL_AddTimer(MOUSE_EMULATION_POLLING_INTERVAL, SdlInputHandler::mouseEmulationTimerCallback, state);

                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Mouse emulation active");
                    Session::get()->notifyMouseEmulationMode(true);
                }
            }
        }
        else if (state->mouseEmulationTimer != 0) {
            if (event->button == SDL_CONTROLLER_BUTTON_A) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_B) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_X) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X1);
            }
            else if (event->button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X2);
            }
        }
    }

    // Handle Start+Select+L1+R1 as a gamepad quit combo
    if (state->buttons == (PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG) && qgetenv("NO_GAMEPAD_QUIT") != "1") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quit gamepad button combo");

        // Push a quit event to the main loop
        SDL_Event event;
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);

        // Clear buttons down on this gamepad
#ifdef ARBS_W
        if (!state->arbsHidden)  // B2c 2-1: a hidden pad's quit combo acts locally only
#endif
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
        return;
    }

    // Handle Select+L1+R1+X as a gamepad overlay combo
    if (state->buttons == (BACK_FLAG | LB_FLAG | RB_FLAG | X_FLAG)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected stats toggle gamepad combo");

        // Toggle the stats overlay
        Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayDebug,
                                                            !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug));

        // Clear buttons down on this gamepad
#ifdef ARBS_W
        if (!state->arbsHidden)  // B2c 2-1: the overlay toggle is local; nothing goes to the host for a hidden pad
#endif
        LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                   0, 0, 0, 0, 0, 0, 0);
        return;
    }

    // Only send the gamepad state to the host if it's not in mouse emulation mode
    if (state->mouseEmulationTimer == 0) {
        sendGamepadState(state);
    }
}

#if SDL_VERSION_ATLEAST(2, 0, 14)

void SdlInputHandler::handleControllerSensorEvent(SDL_ControllerSensorEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }
#ifdef ARBS_W
    if (state->arbsHidden) {
        return;  // B2c 2-1: no motion from a hidden pad
    }
#endif

    switch (event->sensor) {
    case SDL_SENSOR_ACCEL:
        if (state->accelReportPeriodMs &&
                SDL_TICKS_PASSED(event->timestamp, state->lastAccelEventTime + state->accelReportPeriodMs) &&
                memcmp(event->data, state->lastAccelEventData, sizeof(event->data)) != 0) {
            memcpy(state->lastAccelEventData, event->data, sizeof(event->data));
            state->lastAccelEventTime = event->timestamp;

            LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_ACCEL, event->data[0], event->data[1], event->data[2]);
        }
        break;
    case SDL_SENSOR_GYRO:
        if (state->gyroReportPeriodMs &&
                SDL_TICKS_PASSED(event->timestamp, state->lastGyroEventTime + state->gyroReportPeriodMs) &&
                memcmp(event->data, state->lastGyroEventData, sizeof(event->data)) != 0) {
            memcpy(state->lastGyroEventData, event->data, sizeof(event->data));
            state->lastGyroEventTime = event->timestamp;

            // Convert rad/s to deg/s
            LiSendControllerMotionEvent((uint8_t)state->index, LI_MOTION_TYPE_GYRO,
                                        event->data[0] * 57.2957795f,
                                        event->data[1] * 57.2957795f,
                                        event->data[2] * 57.2957795f);
        }
        break;
    }
}

void SdlInputHandler::handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }
#ifdef ARBS_W
    if (state->arbsHidden) {
        return;  // B2c 2-1: no touch from a hidden pad
    }
#endif

    uint8_t eventType;
    switch (event->type) {
    case SDL_CONTROLLERTOUCHPADDOWN:
        eventType = LI_TOUCH_EVENT_DOWN;
        break;
    case SDL_CONTROLLERTOUCHPADUP:
        eventType = LI_TOUCH_EVENT_UP;
        break;
    case SDL_CONTROLLERTOUCHPADMOTION:
        eventType = LI_TOUCH_EVENT_MOVE;
        break;
    default:
        return;
    }

    LiSendControllerTouchEvent2((uint8_t)state->index, eventType,
                                (uint8_t)event->touchpad, event->finger,
                                event->x, event->y, event->pressure);
}

#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)

void SdlInputHandler::handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event)
{
    GamepadState* state = findStateForGamepad(event->which);
    if (state == NULL) {
        return;
    }
#ifdef ARBS_W
    if (state->arbsArrivalSkipped) {
        // Single mode + APEX first: this pad is not reported, its battery must not replace the APEX one on number 0
        return;
    }
    if (state->arbsHidden) {
        return;  // B2c 2-1: no battery from a hidden pad
    }
#endif

    sendGamepadBatteryState(state, event->level);
}

#endif

void SdlInputHandler::handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event)
{
    GamepadState* state;

    if (event->type == SDL_CONTROLLERDEVICEADDED) {
        int i;
        const char* name;
        SDL_GameController* controller;
        const char* mapping;
        char guidStr[33];
        uint32_t hapticCaps;

        controller = SDL_GameControllerOpen(event->which);
        if (controller == NULL) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to open gamepad: %s",
                         SDL_GetError());
#ifdef ARBS_W
            // B5 D-B5-4: a 37d7/2502 pad that cannot be opened must not keep the APEX-only hiding pending
            if (ArbsBridge::onArrivalDropped(SDL_JoystickGetDeviceInstanceID(event->which), SDL_JoystickGetDeviceVendor(event->which),
                                             SDL_JoystickGetDeviceProduct(event->which), "열기 실패")) {
                arbsSyncHidden(nullptr);
            }
#endif
            return;
        }

        // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
        // gamepad in rare cases, because SDL doesn't fixup the device index in
        // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
        // before we've processed the add event.
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            if (m_GamepadState[i].controller == controller) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Received duplicate add event for controller index: %d",
                            event->which);
                SDL_GameControllerClose(controller);
                return;
            }
        }

        // We used to use SDL_GameControllerGetPlayerIndex() here but that
        // can lead to strange issues due to bugs in Windows where an Xbox
        // controller will join as player 2, even though no player 1 controller
        // is connected at all. This pretty much screws any attempt to use
        // the gamepad in single player games, so just assign them in order from 0.
        i = 0;

        for (; i < MAX_GAMEPADS; i++) {
            SDL_assert(m_GamepadState[i].controller != controller);
            if (m_GamepadState[i].controller == NULL) {
                // Found an empty slot
                break;
            }
        }

#ifdef ARBS_W
        // ARBS-W B2b 2-2: APEX first numbering. VID/PID are read before the slot choice. Multi-controller mode + setting
        // on: a 37d7/2502 pad takes slot 0 (the pad in 0 moves to the first free slot), other pads start at 1 while a
        // 37d7/2502 device is in the SDL list. Setting off or single mode: the upstream choice above.
        const bool arbsPadExact = ArbsBridge::isExactPad(SDL_GameControllerGetVendor(controller), SDL_GameControllerGetProduct(controller));
        int arbsMoveTo = -1;
        {
            bool occupied[MAX_GAMEPADS];
            for (int j = 0; j < MAX_GAMEPADS; j++) {
                occupied[j] = m_GamepadState[j].controller != NULL;
            }
            const bool slot0Exact = occupied[0] &&
                    ArbsBridge::isExactPad(SDL_GameControllerGetVendor(m_GamepadState[0].controller),
                                           SDL_GameControllerGetProduct(m_GamepadState[0].controller));
            // B2c 2-1: APEX-only on = APEX first numbering whatever the APEX first setting says
            const arbsw::SlotPlan plan = arbsw::planSlot(occupied, MAX_GAMEPADS, m_MultiController,
                                                         arbsw::apexFirstEffective(m_ArbsApexFirst, m_ArbsApexOnly), slot0Exact,
                                                         arbsPadExact, ArbsBridge::sdlListHasExact());
            i = plan.slot >= 0 ? plan.slot : MAX_GAMEPADS;
            arbsMoveTo = plan.moveTo;
        }
#endif

        if (i == MAX_GAMEPADS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "No open gamepad slots found!");
#ifdef ARBS_W
            const bool arbsDropped = ArbsBridge::onArrivalDropped(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)),
                                                                  SDL_GameControllerGetVendor(controller),
                                                                  SDL_GameControllerGetProduct(controller), "빈 칸 없음");
#endif
            SDL_GameControllerClose(controller);
#ifdef ARBS_W
            if (arbsDropped) {
                arbsSyncHidden(nullptr);  // B5 D-B5-4
            }
#endif
            return;
        }

        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(controller)),
                                  guidStr, sizeof(guidStr));
        if (m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive))
        {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Skipping ignored device with GUID: %s",
                        guidStr);
#ifdef ARBS_W
            const bool arbsDropped = ArbsBridge::onArrivalDropped(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)),
                                                                  SDL_GameControllerGetVendor(controller),
                                                                  SDL_GameControllerGetProduct(controller), "무시 GUID");
#endif
            SDL_GameControllerClose(controller);
#ifdef ARBS_W
            if (arbsDropped) {
                arbsSyncHidden(nullptr);  // B5 D-B5-4
            }
#endif
            return;
        }

#ifdef ARBS_W
        if (arbsMoveTo >= 0) {
            // The APEX arrived after another pad took slot 0: that pad moves (its host report is sent below, before ours)
            arbsMovePad(0, arbsMoveTo);
        }
#endif

        state = &m_GamepadState[i];
        if (m_MultiController) {
            state->index = i;

#if SDL_VERSION_ATLEAST(2, 0, 12)
            // This will change indicators on the controller to show the assigned
            // player index. For Xbox 360 controllers, that means updating the LED
            // ring to light up the corresponding quadrant for this player.
            SDL_GameControllerSetPlayerIndex(controller, state->index);
#endif
        }
        else {
            // Always player 1 in single controller mode
            state->index = 0;
        }

        state->controller = controller;
        state->jsId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(state->controller));

        hapticCaps = 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        hapticCaps |= SDL_GameControllerHasRumble(controller) ? ML_HAPTIC_GC_RUMBLE : 0;
        hapticCaps |= SDL_GameControllerHasRumbleTriggers(controller) ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#elif SDL_VERSION_ATLEAST(2, 0, 9)
        // Perform a tiny rumbles to see if haptics are supported.
        // NB: We cannot use zeros for rumble intensity or SDL will not actually call the JS driver
        // and we'll get a (potentially false) success value returned.
        hapticCaps |= SDL_GameControllerRumble(controller, 1, 1, 1) == 0 ? ML_HAPTIC_GC_RUMBLE : 0;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        hapticCaps |= SDL_GameControllerRumbleTriggers(controller, 1, 1, 1) == 0 ? ML_HAPTIC_GC_TRIGGER_RUMBLE : 0;
#endif
#else
        state->haptic = SDL_HapticOpenFromJoystick(SDL_GameControllerGetJoystick(state->controller));
        state->hapticEffectId = -1;
        state->hapticMethod = GAMEPAD_HAPTIC_METHOD_NONE;
        if (state->haptic != nullptr) {
            // Query for supported haptic effects
            hapticCaps = SDL_HapticQuery(state->haptic);
            hapticCaps |= SDL_HapticRumbleSupported(state->haptic) ?
                            ML_HAPTIC_SIMPLE_RUMBLE : 0;

            if ((SDL_HapticQuery(state->haptic) & SDL_HAPTIC_LEFTRIGHT) == 0) {
                if (SDL_HapticRumbleSupported(state->haptic)) {
                    if (SDL_HapticRumbleInit(state->haptic) == 0) {
                        state->hapticMethod = GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE;
                    }
                }
                if (state->hapticMethod == GAMEPAD_HAPTIC_METHOD_NONE) {
                    SDL_HapticClose(state->haptic);
                    state->haptic = nullptr;
                }
            } else {
                state->hapticMethod = GAMEPAD_HAPTIC_METHOD_LEFTRIGHT;
            }
        }
        else {
            hapticCaps = 0;
        }
#endif

        mapping = SDL_GameControllerMapping(state->controller);
        name = SDL_GameControllerName(state->controller);

        uint16_t vendorId = SDL_GameControllerGetVendor(state->controller);
        uint16_t productId = SDL_GameControllerGetProduct(state->controller);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Gamepad %d (player %d) is: %s (VID/PID: 0x%.4x/0x%.4x) (haptic capabilities: 0x%x) (mapping: %s -> %s)",
                    i,
                    state->index,
                    name != nullptr ? name : "<null>",
                    vendorId,
                    productId,
                    hapticCaps,
                    guidStr,
                    mapping != nullptr ? mapping : "<null>");
        if (mapping != nullptr) {
            SDL_free((void*)mapping);
        }

#ifdef ARBS_W
        // ARBS-W: APEX 2.4G dongle? B2b 2-1: SDL VID/PID 37D7/2502 + one IF2 binds at once (and replaces a presumed
        // binding); the XInput fallback 045E/028E is only checked 1000 ms after the first pad, when no 37D7/2502 device
        // is in the SDL list. Bound -> IF2 opened, ARBS Core attached, reported as a PS controller with PCM haptics.
        const ArbsBind arbsBind = ArbsBridge::onGamepadAdded(state->index, state->jsId, vendorId, productId);
        const bool arbsApex = arbsBind.boundJs == state->jsId;
        bool arbsSent = true;
        // B2c 2-1: APEX-only mode - not the APEX while the APEX is bound or a 37d7/2502 pad is still pending -> hidden
        state->arbsHidden = arbsw::hideOther(m_ArbsApexOnly, ArbsBridge::isApexJoystick(state->jsId), ArbsBridge::apexBound(),
                                             ArbsBridge::exactPending());
#endif

        // Add this gamepad to the gamepad mask
        if (m_MultiController) {
            // NB: Don't assert that it's unset here because we will already
            // have the mask set for initially attached gamepads to avoid confusing
            // apps running on the host.
#ifdef ARBS_W
            if (state->arbsHidden) {
                // B2c 2-1: a hidden pad gets no mask bit (also cleared if the start mask had it)
                m_GamepadMask &= ~(1 << state->index);
            }
            else
#endif
            m_GamepadMask |= (1 << state->index);
        }
        else {
            SDL_assert(m_GamepadMask == 0x1);
        }

        SDL_JoystickPowerLevel powerLevel = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(state->controller));

#if SDL_VERSION_ATLEAST(2, 0, 14)
        // On SDL 2.0.14 and later, we can provide enhanced controller information to the host PC
        // for it to use as a hint for the type of controller to emulate.
        uint32_t supportedButtonFlags = 0;
        for (int i = 0; i < (int)SDL_arraysize(k_ButtonMap); i++) {
            if (SDL_GameControllerHasButton(state->controller, (SDL_GameControllerButton)i)) {
                supportedButtonFlags |= k_ButtonMap[i];
            }
        }

        uint32_t capabilities = 0;
        if (SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
            SDL_GameControllerGetBindForAxis(state->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT).bindType == SDL_CONTROLLER_BINDTYPE_AXIS) {
            // We assume these are analog triggers if the binding is to an axis rather than a button
            capabilities |= LI_CCAP_ANALOG_TRIGGERS;
        }
        if (hapticCaps & ML_HAPTIC_GC_RUMBLE) {
            capabilities |= LI_CCAP_RUMBLE;
        }
        if (hapticCaps & ML_HAPTIC_GC_TRIGGER_RUMBLE) {
            capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        }
        if (SDL_GameControllerGetNumTouchpads(state->controller) > 0) {
            capabilities |= LI_CCAP_TOUCHPAD;
            if (SDL_GameControllerGetNumTouchpads(state->controller) > 1) {
                capabilities |= LI_CCAP_DUAL_TOUCHPAD;
            }
        }
        if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_ACCEL)) {
            capabilities |= LI_CCAP_ACCEL;
        }
        if (SDL_GameControllerHasSensor(state->controller, SDL_SENSOR_GYRO)) {
            capabilities |= LI_CCAP_GYRO;
        }
        if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN || SDL_VERSION_ATLEAST(2, 24, 0)) {
            capabilities |= LI_CCAP_BATTERY_STATE;
        }
        if (SDL_GameControllerHasLED(state->controller)) {
            capabilities |= LI_CCAP_RGB_LED;
        }

        uint8_t type;
        switch (SDL_GameControllerGetType(state->controller)) {
        case SDL_CONTROLLER_TYPE_XBOX360:
        case SDL_CONTROLLER_TYPE_XBOXONE:
            type = LI_CTYPE_XBOX;
            break;
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            type = LI_CTYPE_PS;
            break;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
            type = LI_CTYPE_NINTENDO;
            break;
        default:
            // These Steam Controller VID/PID combos come from SDL's controller_list.h
            // TODO: Use SDL_GAMEPAD_TYPE_STEAM on SDL 3.6+
            if (vendorId == 0x28de) {
                switch (productId) {
                case 0x1101:
                case 0x1102:
                case 0x1105:
                case 0x1106:
                case 0x1142:
                case 0x1201:
                case 0x1202:
                case 0x1205:
                case 0x1302:
                case 0x1303:
                case 0x1304:
                case 0x1305:
                    type = LI_CTYPE_STEAM;
                    break;
                default:
                    type = LI_CTYPE_UNKNOWN;
                    break;
                }
            }
            else {
                type = LI_CTYPE_UNKNOWN;
            }
            break;
        }

#ifdef ARBS_W
        // The pad's own arrival info, for re-reports (re-bind / slot move)
        state->arbsType = type;
        state->arbsButtons = supportedButtonFlags;
        state->arbsCaps = capabilities;
        if (arbsApex) {
            // DualSense-style report: PS type + analog triggers + rumble + PCM haptics (0x8000), no trigger rumble
            type = ArbsBridge::apexType();
            capabilities = ArbsBridge::apexCaps(capabilities);
        }
#endif

        // If this is a PlayStation controller that doesn't have a touchpad button mapped,
        // we'll allow the Select+PS button combo to act as the touchpad.
        state->clickpadButtonEmulationEnabled =
#if SDL_VERSION_ATLEAST(2, 0, 14)
            SDL_GameControllerGetBindForButton(state->controller, SDL_CONTROLLER_BUTTON_TOUCHPAD).bindType == SDL_CONTROLLER_BINDTYPE_NONE &&
#endif
            type == LI_CTYPE_PS;

#ifdef ARBS_W
        // Host report order (B2b analysis 2-2 channel rule): the moved pad first (its new number, mask still holds 0),
        // then the pad that lost the APEX binding, then this pad (removal MC first if its number was already allocated).
        // The APEX arrival goes through the stream-live gate (once per attach); rc goes to the ARBS log.
        // B2c 2-1 (APEX-only): first hide / show the other pads to match the new binding (hidden pads send nothing below).
        if (m_ArbsApexOnly) {
            if (state->arbsHidden) {
                char line[200];
                std::snprintf(line, sizeof(line),
                              "ARBS-W: APEX 단독: 다른 패드 VID/PID %04x/%04x 숨김(번호 %d) - 호스트에 도착·입력·모션·터치·배터리를 보내지 않음",
                              vendorId, productId, state->index);
                ArbsBridge::log(line);
            }
            arbsSyncHidden(state);
        }
        if (arbsMoveTo >= 0) {
            arbsSendArrival(&m_GamepadState[arbsMoveTo], ArbsBridge::isApexJoystick(m_GamepadState[arbsMoveTo].jsId));
        }
        if (arbsBind.unboundJs != ArbsBind::kNone) {
            ArbsBind lost;
            lost.unboundJs = arbsBind.unboundJs;
            arbsApplyBind(lost);
        }
        arbsSent = arbsSendArrival(state, arbsApex);
#else
        LiSendControllerArrivalEvent(state->index, m_GamepadMask, type, supportedButtonFlags, capabilities);
#endif
#else

        // Send an empty event to tell the PC we've arrived
        sendGamepadState(state);
#endif

        // Send a power level if it's known at this time
#ifdef ARBS_W
        if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN && arbsSent) {
#else
        if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN) {
#endif
            sendGamepadBatteryState(state, powerLevel);
        }
    }
    else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        state = findStateForGamepad(event->which);
        if (state != NULL) {
            if (state->mouseEmulationTimer != 0) {
                Session::get()->notifyMouseEmulationMode(false);
                SDL_RemoveTimer(state->mouseEmulationTimer);
            }

            SDL_GameControllerClose(state->controller);

#if !SDL_VERSION_ATLEAST(2, 0, 9)
            if (state->haptic != nullptr) {
                SDL_HapticClose(state->haptic);
            }
#endif

#ifdef ARBS_W
            // B2b 2-4: only the bound APEX detaches the engine (stop sequence, IF2 closed); a remaining 37D7/2502 pad
            // is bound at once (reported after this removal)
            const ArbsBind arbsBind = ArbsBridge::onGamepadRemoved(state->jsId);
            if (state->arbsHidden) {
                // B2c 2-1: the host never knew this pad - no mask change, no final MC packet, no ledger change
                char line[160];
                std::snprintf(line, sizeof(line), "ARBS-W: APEX 단독: 숨긴 패드 제거(SDL, 번호 %d) - 호스트 보고 없음", state->index);
                ArbsBridge::log(line);
                SDL_memset(state, 0, sizeof(*state));
                arbsApplyBind(arbsBind);
                return;
            }
            // B2c 2-1: pads shown again after this removal get their mask bits before the removal MC below (channel rule)
            arbsPrepareShow(state);
#endif

            // Remove this from the gamepad mask in MC-mode
            if (m_MultiController) {
                SDL_assert(m_GamepadMask & (1 << state->index));
                m_GamepadMask &= ~(1 << state->index);
            }
            else {
                SDL_assert(m_GamepadMask == 0x1);
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Gamepad %d is gone",
                        state->index);

            // Send a final event to let the PC know this gamepad is gone
            LiSendMultiControllerEvent(state->index, m_GamepadMask,
                                       0, 0, 0, 0, 0, 0, 0);
#ifdef ARBS_W
            if (m_MultiController) {
                m_ArbsHost.removed(state->index);
            }
#endif

            // Clear all remaining state from this slot
            SDL_memset(state, 0, sizeof(*state));
#ifdef ARBS_W
            arbsApplyBind(arbsBind);
#endif
        }
    }
}

#ifdef ARBS_W
// ------------------------------------------------------------------------------------------------ ARBS-W B2b
static bool arbsClickpadEmulation(GamepadState* state, bool psType)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    return psType &&
           SDL_GameControllerGetBindForButton(state->controller, SDL_CONTROLLER_BUTTON_TOUCHPAD).bindType == SDL_CONTROLLER_BINDTYPE_NONE;
#else
    return psType;
#endif
}

void SdlInputHandler::arbsHandleApexCheck(int generation)
{
    arbsApplyBind(ArbsBridge::onApexCheck(generation));
}

void SdlInputHandler::arbsApplyBind(const ArbsBind& bind)
{
    if (bind.unboundJs != ArbsBind::kNone) {
        GamepadState* lost = findStateForGamepad(bind.unboundJs);
        if (lost != nullptr) {
            // Multi-controller mode: its number held the APEX (PS/PCM) arrival -> removal MC + its own arrival again.
            // Single mode: nothing (number 0 stays PS/PCM for the new APEX).
            // B2c 2-1: APEX-only mode -> the pad that lost the binding is hidden instead (removal only, arbsSyncHidden).
            if (!m_ArbsApexOnly) {
                arbsRunSteps(arbsw::planUnbind(m_ArbsHost, m_MultiController, lost->index, (uint16_t)m_GamepadMask), lost);
            }
            lost->clickpadButtonEmulationEnabled = arbsClickpadEmulation(lost, lost->arbsType == LI_CTYPE_PS);
        }
    }
    if (m_ArbsApexOnly) {
        // B2c 2-1: binding changed (bind / unbind / bind failure) -> hide or show the other pads before the APEX arrival
        arbsSyncHidden(nullptr);
    }
    if (bind.boundJs != ArbsBind::kNone) {
        GamepadState* apex = findStateForGamepad(bind.boundJs);
        if (apex != nullptr) {
            if (apex->arbsHidden) {
                // B2c 2-1: a hidden 37d7/2502 pad became the APEX (the bound one was removed) -> visible, mask bit back
                // (already set before the removal MC by arbsPrepareShow in multi-controller mode)
                apex->arbsHidden = false;
                if (m_MultiController) {
                    m_GamepadMask |= (1 << apex->index);
                }
            }
            // Already reported as a normal pad (delayed presumed match / re-bind after removal): report it as the APEX.
            arbsSendArrival(apex, true);
            apex->clickpadButtonEmulationEnabled = arbsClickpadEmulation(apex, true);
        }
    }
}

void SdlInputHandler::arbsSyncHidden(GamepadState* skip)
{
    if (!m_ArbsApexOnly) {
        return;
    }
    const bool bound = ArbsBridge::apexBound();
    const bool pending = ArbsBridge::exactPending();
    if (!arbsw::hideOther(true, false, bound, pending) && m_MultiController) {
        // Provisional decision D1 (planner has not ruled, analysis recommendation): the APEX left and slot 0 is free ->
        // the hidden pad in the lowest slot becomes number 0 (1P-only games; removal MC and arrival share channel 0)
        bool occupied[MAX_GAMEPADS];
        bool hidden[MAX_GAMEPADS];
        for (int i = 0; i < MAX_GAMEPADS; i++) {
            occupied[i] = m_GamepadState[i].controller != NULL;
            hidden[i] = occupied[i] && m_GamepadState[i].arbsHidden && &m_GamepadState[i] != skip;
        }
        const int mover = arbsw::pickShowMover(occupied, hidden, MAX_GAMEPADS);
        if (mover >= 0) {
            arbsMovePad(mover, 0, false);
        }
    }
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        GamepadState* s = &m_GamepadState[i];
        if (s->controller == NULL || s == skip) {
            continue;
        }
        const bool apex = ArbsBridge::isApexJoystick(s->jsId);
        const bool want = arbsw::hideOther(true, apex, bound, pending);
        if (want && !s->arbsHidden) {
            arbsHidePad(s);
        }
        else if (!want && s->arbsHidden && !apex) {
            arbsShowPad(s);
        }
    }
}

void SdlInputHandler::arbsHidePad(GamepadState* state)
{
    // Provisional decision D2: a hidden pad sends nothing, mouse emulation included (its events would reach the host)
    if (state->mouseEmulationTimer != 0) {
        SDL_RemoveTimer(state->mouseEmulationTimer);
        state->mouseEmulationTimer = 0;
        Session::get()->notifyMouseEmulationMode(false);
    }
    if (m_MultiController) {
        m_GamepadMask &= ~(1 << state->index);
    }
    char line[200];
    for (const arbsw::HostStep& s : arbsw::planHide(m_ArbsHost, m_MultiController, state->index, (uint16_t)m_GamepadMask)) {
        LiSendMultiControllerEvent(s.index, s.mask, 0, 0, 0, 0, 0, 0, 0);
        std::snprintf(line, sizeof(line), "ARBS-W: APEX 단독: 호스트 보고 - 번호 %d 제거(MC 마스크 0x%x), 이 패드는 숨김", s.index, s.mask);
        ArbsBridge::log(line);
        m_ArbsHost.sent(s, state->jsId);
    }
    state->arbsHidden = true;
    std::snprintf(line, sizeof(line),
                  "ARBS-W: APEX 단독: 다른 패드 VID/PID %04x/%04x 숨김(번호 %d) - 호스트에 도착·입력·모션·터치·배터리를 보내지 않음",
                  SDL_GameControllerGetVendor(state->controller), SDL_GameControllerGetProduct(state->controller), state->index);
    ArbsBridge::log(line);
}

void SdlInputHandler::arbsShowPad(GamepadState* state)
{
    state->arbsHidden = false;
    if (m_MultiController) {
        m_GamepadMask |= (1 << state->index);
    }
    char line[200];
    std::snprintf(line, sizeof(line), "ARBS-W: APEX 단독: APEX 없음(빠짐·연결 실패) - 숨긴 패드 VID/PID %04x/%04x 다시 보고(번호 %d)",
                  SDL_GameControllerGetVendor(state->controller), SDL_GameControllerGetProduct(state->controller), state->index);
    ArbsBridge::log(line);
    arbsRunSteps(arbsw::planUnhide(m_ArbsHost, m_MultiController, state->index, state->jsId, (uint16_t)m_GamepadMask), state);
    const SDL_JoystickPowerLevel powerLevel = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(state->controller));
    if (powerLevel != SDL_JOYSTICK_POWER_UNKNOWN) {
        sendGamepadBatteryState(state, powerLevel);
    }
    // The local state was kept while hidden: send it once so the host starts from the real buttons/axes
    sendGamepadState(state);
}

void SdlInputHandler::arbsPrepareShow(GamepadState* removed)
{
    if (!m_ArbsApexOnly || !m_MultiController || removed->arbsHidden) {
        return;
    }
    // State after this removal (the bridge has already updated the binding)
    const bool bound = ArbsBridge::apexBound();
    const bool pending = ArbsBridge::exactPending();
    const bool hiding = arbsw::hideOther(true, false, bound, pending);
    bool occupied[MAX_GAMEPADS];
    bool hidden[MAX_GAMEPADS];
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        occupied[i] = m_GamepadState[i].controller != NULL && &m_GamepadState[i] != removed;
        hidden[i] = occupied[i] && m_GamepadState[i].arbsHidden;
    }
    const int mover = hiding ? -1 : arbsw::pickShowMover(occupied, hidden, MAX_GAMEPADS);
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (!hidden[i] || i == mover) {
            continue;
        }
        // Shown after the removal: all hidden pads (APEX gone), or only a hidden pad that just became the APEX. Their
        // arrivals go on other channels than the removal MC, so that MC must already carry their bits. The pad moving to
        // slot 0 (D1) is not added: its arrival shares channel 0 with the removal MC, which must free number 0.
        if (!hiding || ArbsBridge::isApexJoystick(m_GamepadState[i].jsId)) {
            m_GamepadMask |= (1 << m_GamepadState[i].index);
        }
    }
}

bool SdlInputHandler::arbsSendArrival(GamepadState* state, bool asApex)
{
    if (state->arbsHidden) {
        return false;  // B2c 2-1: APEX-only mode - nothing goes to the host for a hidden pad
    }
    const uint16_t vid = SDL_GameControllerGetVendor(state->controller);
    const uint16_t pid = SDL_GameControllerGetProduct(state->controller);
    // B2c 2-1: with APEX-only on, hiding replaces the B2b single-mode arrival skip (a visible pad is always reported)
    if (!m_ArbsApexOnly &&
        arbsw::skipArrivalSingle(m_MultiController, m_ArbsApexFirst, ArbsBridge::isExactPad(vid, pid), asApex,
                                 ArbsBridge::apexBound(), ArbsBridge::sdlListHasExact())) {
        // Risk K1: the host keeps the first arrival of number 0, so it must be the APEX (PS/PCM) one.
        state->arbsArrivalSkipped = true;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "ARBS-W: APEX 우선(단일 컨트롤러 모드): APEX 가 아닌 패드 VID/PID %04x/%04x 의 도착 보고 생략 - 0번은 APEX 도착이 만듦, 입력은 0번에 합쳐짐",
                      vid, pid);
        ArbsBridge::log(line);
        return false;
    }
    state->arbsArrivalSkipped = false;
    arbsRunSteps(arbsw::planArrival(m_ArbsHost, m_MultiController, arbsw::apexFirstEffective(m_ArbsApexFirst, m_ArbsApexOnly),
                                    state->index, asApex, state->jsId, (uint16_t)m_GamepadMask),
                 state);
    return true;
}

void SdlInputHandler::arbsRunSteps(const std::vector<arbsw::HostStep>& steps, GamepadState* state)
{
    char line[256];
    for (const arbsw::HostStep& s : steps) {
        if (s.kind == arbsw::HostStep::Kind::Remove) {
            LiSendMultiControllerEvent(s.index, s.mask, 0, 0, 0, 0, 0, 0, 0);
            std::snprintf(line, sizeof(line), "ARBS-W: 호스트 보고 - 번호 %d 제거(MC 마스크 0x%x) 후 다시 도착 보고", s.index, s.mask);
            ArbsBridge::log(line);
        }
        else if (s.apex) {
            if (!m_MultiController && m_ArbsHost.role(0) == arbsw::HostRole::Normal) {
                ArbsBridge::log("ARBS-W 경고: 0번이 이미 일반 패드 도착으로 만들어져 있음(APEX 우선 끔) - 호스트가 APEX(PS·PCM) 도착을 무시할 수 있음");
            }
            ArbsBridge::sendApexArrival(s.index, s.mask, ArbsBridge::apexType(), state->arbsButtons, ArbsBridge::apexCaps(state->arbsCaps));
        }
        else {
            const int rc = LiSendControllerArrivalEvent(s.index, s.mask, state->arbsType, state->arbsButtons, state->arbsCaps);
            std::snprintf(line, sizeof(line), "ARBS-W: 일반 패드 도착 보고 번호 %d 타입 %u caps 0x%x 마스크 0x%x rc=%d", s.index,
                          (unsigned)state->arbsType, (unsigned)state->arbsCaps, s.mask, rc);
            ArbsBridge::log(line);
        }
        m_ArbsHost.sent(s, state->jsId);
    }
}

void SdlInputHandler::arbsMovePad(int from, int to, bool forApex)
{
    GamepadState* x = &m_GamepadState[from];
    // Risk K3: the mouse emulation timer holds a pointer to this slot -> stop it before the slot moves
    if (x->mouseEmulationTimer != 0) {
        SDL_RemoveTimer(x->mouseEmulationTimer);
        x->mouseEmulationTimer = 0;
        Session::get()->notifyMouseEmulationMode(false);
    }
    // Risk K4: multi-controller invariant m_GamepadState[i].index == i
    m_GamepadState[to] = *x;
    m_GamepadState[to].index = to;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_GameControllerSetPlayerIndex(m_GamepadState[to].controller, to);
#endif
    SDL_memset(x, 0, sizeof(*x));
    // Bit [from] stays set (the APEX takes that number); the moved pad's bit is added (B2c: not for a hidden pad)
    if (!m_GamepadState[to].arbsHidden) {
        m_GamepadMask |= (1 << to);
    }
    ArbsBridge::onGamepadIndexChanged(m_GamepadState[to].jsId, to);
    char line[160];
    if (forApex) {
        std::snprintf(line, sizeof(line), "ARBS-W: APEX 우선 - %d번 패드를 %d번으로 옮김(APEX 가 %d번)", from, to, from);
    }
    else {
        std::snprintf(line, sizeof(line), "ARBS-W: APEX 단독: APEX 빠짐 - 숨긴 %d번 패드를 %d번으로 옮김(잠정 결정 D1)", from, to);
    }
    ArbsBridge::log(line);
}
#endif

void SdlInputHandler::handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event)
{
    SDL_assert(event->type == SDL_JOYDEVICEADDED);

    if (!SDL_IsGameController(event->which)) {
        char guidStr[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(event->which),
                                  guidStr, sizeof(guidStr));
        const char* name = SDL_JoystickNameForIndex(event->which);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Joystick discovered with no mapping: %s %s",
                    name ? name : "<UNKNOWN>",
                    guidStr);
        SDL_Joystick* joy = SDL_JoystickOpen(event->which);
        if (joy != nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Number of axes: %d | Number of buttons: %d | Number of hats: %d",
                        SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy),
                        SDL_JoystickNumHats(joy));
            SDL_JoystickClose(joy);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to open joystick for query: %s",
                        SDL_GetError());
        }
    }
}

void SdlInputHandler::rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        SDL_GameControllerRumble(m_GamepadState[controllerNumber].controller, lowFreqMotor, highFreqMotor, 30000);
    }
#else
    // Check if the controller supports haptics (and if the controller exists at all)
    SDL_Haptic* haptic = m_GamepadState[controllerNumber].haptic;
    if (haptic == nullptr) {
        return;
    }

    // Stop the last effect we played
    if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_LEFTRIGHT) {
        if (m_GamepadState[controllerNumber].hapticEffectId >= 0) {
            SDL_HapticDestroyEffect(haptic, m_GamepadState[controllerNumber].hapticEffectId);
            m_GamepadState[controllerNumber].hapticEffectId = -1;
        }
    } else if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE) {
        SDL_HapticRumbleStop(haptic);
    }

    // If this callback is telling us to stop both motors, don't bother queuing a new effect
    if (lowFreqMotor == 0 && highFreqMotor == 0) {
        return;
    }

    if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_LEFTRIGHT) {
        SDL_HapticEffect effect;
        SDL_memset(&effect, 0, sizeof(effect));
        effect.type = SDL_HAPTIC_LEFTRIGHT;

        // The effect should last until we are instructed to stop or change it
        effect.leftright.length = SDL_HAPTIC_INFINITY;

        // SDL haptics range from 0-32767 but XInput uses 0-65535, so divide by 2 to correct for SDL's scaling
        effect.leftright.large_magnitude = lowFreqMotor / 2;
        effect.leftright.small_magnitude = highFreqMotor / 2;

        // Play the new effect
        m_GamepadState[controllerNumber].hapticEffectId = SDL_HapticNewEffect(haptic, &effect);
        if (m_GamepadState[controllerNumber].hapticEffectId >= 0) {
            SDL_HapticRunEffect(haptic, m_GamepadState[controllerNumber].hapticEffectId, 1);
        }
    } else if (m_GamepadState[controllerNumber].hapticMethod == GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE) {
        SDL_HapticRumblePlay(haptic,
                             std::min(1.0, (GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT*highFreqMotor +
                                            GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT*lowFreqMotor) / 65535.0),
                             SDL_HAPTIC_INFINITY);
    }
#endif
}

void SdlInputHandler::rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        SDL_GameControllerRumbleTriggers(m_GamepadState[controllerNumber].controller, leftTrigger, rightTrigger, 30000);
    }
#endif
}

void SdlInputHandler::setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

    uint16_t reportRateHzLimit;
    if (Utils::getEnvironmentVariableOverride("SENSOR_REPORT_RATE_LIMIT_HZ", &reportRateHzLimit) &&
        reportRateHz > reportRateHzLimit) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Sensor report rate limited to %u Hz by environment variable",
                    reportRateHzLimit);
        reportRateHz = reportRateHzLimit;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        uint32_t reportPeriodMs = reportRateHz ? SDL_max(1000U / reportRateHz, 1U) : 0;

        switch (motionType) {
        case LI_MOTION_TYPE_ACCEL:
            m_GamepadState[controllerNumber].accelReportPeriodMs = reportPeriodMs;
            SDL_GameControllerSetSensorEnabled(m_GamepadState[controllerNumber].controller, SDL_SENSOR_ACCEL, reportRateHz ? SDL_TRUE : SDL_FALSE);
            break;

        case LI_MOTION_TYPE_GYRO:
            m_GamepadState[controllerNumber].gyroReportPeriodMs = reportPeriodMs;
            SDL_GameControllerSetSensorEnabled(m_GamepadState[controllerNumber].controller, SDL_SENSOR_GYRO, reportRateHz ? SDL_TRUE : SDL_FALSE);
            break;
        }
    }
#endif
}

void SdlInputHandler::setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b)
{
    // Make sure the controller number is within our supported count
    if (controllerNumber >= MAX_GAMEPADS) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (m_GamepadState[controllerNumber].controller != nullptr) {
        SDL_GameControllerSetLED(m_GamepadState[controllerNumber].controller, r, g, b);
    }
#endif
}

void SdlInputHandler::setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report){

#if SDL_VERSION_ATLEAST(2, 0, 16)
        // Make sure the controller number is within our supported count
    if (controllerNumber < MAX_GAMEPADS &&
        // and we have a valid controller
        m_GamepadState[controllerNumber].controller != nullptr &&
        // and it's a PS5 controller
        SDL_GameControllerGetType(m_GamepadState[controllerNumber].controller) == SDL_CONTROLLER_TYPE_PS5) {
        SDL_GameControllerSendEffect(m_GamepadState[controllerNumber].controller, report, sizeof(*report));
    }
#endif

    SDL_free(report);
}

QString SdlInputHandler::getUnmappedGamepads()
{
    QString ret;

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (!SDL_IsGameController(i)) {
            char guidStr[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                      guidStr, sizeof(guidStr));
            const char* name = SDL_JoystickNameForIndex(i);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unmapped joystick: %s %s",
                        name ? name : "<UNKNOWN>",
                        guidStr);
            SDL_Joystick* joy = SDL_JoystickOpen(i);
            if (joy != nullptr) {
                int numButtons = SDL_JoystickNumButtons(joy);
                int numHats = SDL_JoystickNumHats(joy);
                int numAxes = SDL_JoystickNumAxes(joy);

                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Number of axes: %d | Number of buttons: %d | Number of hats: %d",
                            numAxes, numButtons, numHats);

                if ((numAxes >= 4 && numAxes <= 8) && numButtons >= 8 && numHats <= 1) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Joystick likely to be an unmapped game controller");
                    if (!ret.isEmpty()) {
                        ret += ", ";
                    }

                    ret += name;
                }

                SDL_JoystickClose(joy);
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to open joystick for query: %s",
                            SDL_GetError());
            }
        }
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    // Flush stale events so they aren't processed by the main session event loop
    SDL_FlushEvents(SDL_JOYDEVICEADDED, SDL_JOYDEVICEREMOVED);
    SDL_FlushEvents(SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMAPPED);

    return ret;
}

int SdlInputHandler::getAttachedGamepadMask()
{
    int count;
    int mask;

    if (!m_MultiController) {
        // Player 1 is always present in non-MC mode
        return 0x1;
    }

    count = mask = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            char guidStr[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),
                                      guidStr, sizeof(guidStr));

            if (!m_IgnoreDeviceGuids.contains(guidStr, Qt::CaseInsensitive) && count < MAX_GAMEPADS)
            {
                mask |= (1 << count++);
            }
        }
    }

#ifdef ARBS_W
    // B2c 2-1: APEX-only + a 37d7/2502 device in the SDL list -> only the APEX bit (the other pads will be hidden)
    const int arbsMask = arbsw::apexOnlyStartMask(true, m_ArbsApexOnly, ArbsBridge::sdlListHasExact(), (uint16_t)mask);
    if (arbsMask != mask) {
        char line[160];
        std::snprintf(line, sizeof(line), "ARBS-W: APEX 단독: 시작 패드 마스크 0x%x -> 0x%x (SDL 목록에 VID/PID 37d7/2502 있음)", mask, arbsMask);
        ArbsBridge::log(line);
        mask = arbsMask;
    }
#endif

    return mask;
}
