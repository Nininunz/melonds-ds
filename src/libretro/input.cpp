/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include "input.hpp"

#include <NDS.h>
#include <glm/gtx/common.hpp>

#include "config.hpp"
#include "environment.hpp"
#include "libretro.hpp"
#include "math.hpp"
#include "screenlayout.hpp"
#include "tracy.hpp"
#include "utils.hpp"

using glm::ivec2;
using glm::ivec3;
using glm::i16vec2;
using glm::mat3;
using glm::vec2;
using glm::vec3;
using glm::uvec2;
using melonds::NDS_SCREEN_SIZE;
using melonds::NDS_SCREEN_HEIGHT;

const struct retro_input_descriptor melonds::input_descriptors[] = {
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_UP,     "Up"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_A,      "A"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_B,      "B"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_START,  "Start"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_R,      "R"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_L,      "L"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_X,      "X"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_Y,      "Y"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_L2,     "Microphone"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_R2,     "Next Screen Layout"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_L3,     "Close lid"},
        {0, RETRO_DEVICE_JOYPAD, 0,                               RETRO_DEVICE_ID_JOYPAD_R3,     "Touch joystick"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,      "Touch joystick X"},
        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,      "Touch joystick Y"},
        {},
};

static bool IsInNdsScreenBounds(ivec2 position) noexcept {
    return glm::all(glm::openBounded(position, ivec2(0), NDS_SCREEN_SIZE<int>));
}

static const char *device_name(unsigned device) {
    switch (device) {
        case RETRO_DEVICE_NONE:
            return "RETRO_DEVICE_NONE";
        case RETRO_DEVICE_JOYPAD:
            return "RETRO_DEVICE_JOYPAD";
        case RETRO_DEVICE_MOUSE:
            return "RETRO_DEVICE_MOUSE";
        case RETRO_DEVICE_KEYBOARD:
            return "RETRO_DEVICE_KEYBOARD";
        case RETRO_DEVICE_LIGHTGUN:
            return "RETRO_DEVICE_LIGHTGUN";
        case RETRO_DEVICE_ANALOG:
            return "RETRO_DEVICE_ANALOG";
        case RETRO_DEVICE_POINTER:
            return "RETRO_DEVICE_POINTER";
        default:
            return "<unknown>";
    }
}

// Not really needed, but libretro requires all retro_* functions to be defined
PUBLIC_SYMBOL void retro_set_controller_port_device(unsigned port, unsigned device) {
    retro::log(RETRO_LOG_DEBUG, "retro_set_controller_port_device(%d, %s)", port, device_name(device));
}

void melonds::HandleInput(InputState& inputState, ScreenLayoutData& screenLayout) noexcept {
    ZoneScopedN("melonds::HandleInput");
    using glm::clamp;
    using glm::all;

    // Read the input from the frontend
    inputState.Update(screenLayout);

    if (inputState.ToggleLidPressed()) {
        NDS::SetLidClosed(!NDS::IsLidClosed());
        retro::log(RETRO_LOG_DEBUG, "%s the lid", NDS::IsLidClosed() ? "Closed" : "Opened");
    }

    if (inputState.CycleLayoutPressed()) {
        // If the user wants to change the active screen layout...
        screenLayout.NextLayout(); // ...update the screen layout to the next in the sequence.
        retro::debug("Switched to screen layout %d of %d", screenLayout.LayoutIndex() + 1, screenLayout.NumberOfLayouts());
    }

    if (inputState.IsTouchingScreen()) {
        uvec2 clampedTouch;
        switch (screenLayout.Layout()) {
            case ScreenLayout::HybridBottom:
                if (screenLayout.HybridSmallScreenLayout() == HybridSideScreenDisplay::One) {
                    // If the touch screen is only shown in the hybrid-screen position...
                    clampedTouch = clamp(inputState.HybridTouchPosition(), ivec2(0), NDS_SCREEN_SIZE<int> - 1);
                    // ...then that's the only transformation we'll use for input.
                    break;
                } else if (!all(glm::openBounded(inputState.TouchPosition(), ivec2(0), NDS_SCREEN_SIZE<int>))) {
                    // The touch screen is shown in both the hybrid and secondary positions.
                    // If the touch input is not within the secondary position's bounds...
                    clampedTouch = clamp(inputState.HybridTouchPosition(), ivec2(0), NDS_SCREEN_SIZE<int> - 1);
                    break;
                }
                [[fallthrough]];
            default:
                clampedTouch = clamp(inputState.TouchPosition(), ivec2(0), NDS_SCREEN_SIZE<int> - 1);
                break;

        }

        NDS::TouchScreen(clampedTouch.x, clampedTouch.y);
    } else if (inputState.ScreenReleased()) {
        NDS::ReleaseScreen();
    }
}

#define ADD_KEY_TO_MASK(key, i, bits) \
    do { \
        if (bits & (1 << (key))) {               \
            ndsInputBits &= ~(1 << (i));           \
        }                                      \
        else {                                 \
            ndsInputBits |= (1 << (i));            \
        }                                      \
    } while (false)

void melonds::InputState::Update(const ScreenLayoutData& screen_layout_data) noexcept {
    ZoneScopedN("melonds::InputState::Update");
    uint32_t retroInputBits; // Input bits from libretro
    uint32_t ndsInputBits = 0xFFF; // Input bits passed to the emulated DS

    if (dirty) {
        cursorTimeout = maxCursorTimeout * 60;
    }

    retro::input_poll();

    if (retro::supports_bitmasks()) {
        retroInputBits = retro::input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    } else {
        retroInputBits = 0;
        for (int i = 0; i < (RETRO_DEVICE_ID_JOYPAD_R3 + 1); i++)
            retroInputBits |= retro::input_state(0, RETRO_DEVICE_JOYPAD, 0, i) ? (1 << i) : 0;
    }

    // Delicate bit manipulation; do not touch!
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_A, 0, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_B, 1, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_SELECT, 2, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_START, 3, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_RIGHT, 4, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_LEFT, 5, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_UP, 6, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_DOWN, 7, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_R, 8, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_L, 9, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_X, 10, retroInputBits);
    ADD_KEY_TO_MASK(RETRO_DEVICE_ID_JOYPAD_Y, 11, retroInputBits);

    NDS::SetKeyMask(ndsInputBits);

    previousToggleLidButton = toggleLidButton;
    toggleLidButton = retroInputBits & (1 << RETRO_DEVICE_ID_JOYPAD_L3);

    previousMicButton = micButton;
    micButton = retroInputBits & (1 << RETRO_DEVICE_ID_JOYPAD_L2);

    previousCycleLayoutButton = cycleLayoutButton;
    cycleLayoutButton = retroInputBits & (1 << RETRO_DEVICE_ID_JOYPAD_R2);

    previousTouch = touch;
    previousTouching = touching;

    ScreenLayout layout = screen_layout_data.Layout();
    if (layout == ScreenLayout::TopOnly) {
        touching = false;
    } else {
        touching = retro::input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
        pointerInput.x = retro::input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        pointerInput.y = retro::input_state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);

        touch = screen_layout_data.TransformPointerInput(pointerInput);
        hybridTouch = screen_layout_data.TransformPointerInputToHybridScreen(pointerInput);
    }

    if (cursorMode == CursorMode::Timeout) {
        if (touching != previousTouching || touch != previousTouch) {
            // If the player moved, pressed, or released the pointer within the past frame...
            cursorTimeout = maxCursorTimeout * 60;
        } else if (cursorTimeout > 0) {
            cursorTimeout--;
        }
    }

    dirty = false;
}

bool melonds::InputState::CursorVisible() const noexcept {
    bool modeAllowsCursor = false;
    switch (config::screen::CursorMode()) {
        case CursorMode::Always:
            modeAllowsCursor = true;
            break;
        case CursorMode::Never:
            modeAllowsCursor = false;
            break;
        case CursorMode::Touching:
            modeAllowsCursor = touching;
            break;
        case CursorMode::Timeout:
            modeAllowsCursor = cursorTimeout > 0;
            break;
    }

    return modeAllowsCursor && !NDS::IsLidClosed() && pointerInput != i16vec2(0);
    // libretro's pointer API returns (0, 0) if the pointer is not over the play area (even if it's still over the window).
    // Theoretically means that the cursor is hidden if the player moves the pointer to the dead center of the screen,
    // but the screen's resolution probably isn't big enough for that to happen in practice.
}