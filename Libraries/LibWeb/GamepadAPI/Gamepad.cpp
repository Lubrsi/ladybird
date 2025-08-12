/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/GamepadAPI/Gamepad.h>
#include <LibWeb/GamepadAPI/GamepadButton.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::GamepadAPI {

GC_DEFINE_ALLOCATOR(Gamepad);

// https://w3c.github.io/gamepad/#dfn-a-new-gamepad
GC::Ref<Gamepad> Gamepad::create(JS::Realm& realm, SDL_JoystickID sdl_joystick_id)
{
    // 1. Let gamepad be a newly created Gamepad instance:
    auto gamepad = realm.create<Gamepad>(realm, sdl_joystick_id);

    //    1. Initialize gamepad's id attribute to an identification string for the gamepad.
    //    FIXME: What is the encoding used by SDL?
    auto const* name = SDL_GetGamepadNameForID(sdl_joystick_id);
    if (name) {
        gamepad->m_id = Utf16String::from_utf8(StringView { name, strlen(name) });
    }

    //    2. Initialize gamepad's index attribute to the result of selecting an unused gamepad index for gamepad.
    //    https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
    //    1. Let navigator be gamepad's relevant global object's Navigator object.
    //    The rest of the steps are implemented in NavigatorGamepad.
    //    NOTE: Gamepad is only exposed on Window.
    auto& window = as<HTML::Window>(HTML::relevant_global_object(gamepad));
    gamepad->m_index = window.navigator()->select_an_unused_gamepad_index({});

    //    3. Initialize gamepad's mapping attribute to the result of selecting a mapping for the gamepad device.
    //    NOTE: This is done in initialize_axes and initialize_buttons, where we'll have the data to determine this.

    //    4. Set gamepad.[[connected]] to true.
    gamepad->m_connected = true;

    //    5. Set gamepad.[[timestamp]] to the current high resolution time given gamepad's relevant global object.
    gamepad->m_timestamp = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(gamepad));

    //    FIXME: 6. Set gamepad.[[axes]] to the result of initializing axes for gamepad.
    gamepad->m_axes.append(0);
    gamepad->m_axes.append(0);
    gamepad->m_axes.append(0);
    gamepad->m_axes.append(0);

    //    7. Set gamepad.[[buttons]] to the result of initializing buttons for gamepad.
    gamepad->initialize_buttons();

    // 2. Return gamepad.
    return gamepad;
}

Gamepad::Gamepad(JS::Realm& realm, SDL_JoystickID sdl_joystick_id)
    : PlatformObject(realm)
    , m_sdl_joystick_id(sdl_joystick_id)
{
    m_sdl_gamepad = SDL_OpenGamepad(m_sdl_joystick_id);
}

void Gamepad::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Gamepad);
    Base::initialize(realm);
}

void Gamepad::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buttons);
}

void Gamepad::finalize()
{
    SDL_CloseGamepad(m_sdl_gamepad);
}

// https://w3c.github.io/gamepad/#dfn-initializing-buttons
void Gamepad::initialize_buttons()
{
    // https://w3c.github.io/gamepad/#dfn-standard-gamepad
    // Type	Index	Location
    // Button   0       Bottom button in right cluster
    //          1       Right button in right cluster
    //          2       Left button in right cluster
    //          3       Top button in right cluster
    //          4       Top left front button
    //          5       Top right front button
    //          6       Bottom left front button
    //          7       Bottom right front button
    //          8       Left button in center cluster
    //          9       Right button in center cluster
    //          10      Left stick pressed button
    //          11      Right stick pressed button
    //          12      Top button in left cluster
    //          13      Bottom button in left cluster
    //          14      Left button in left cluster
    //          15      Right button in left cluster
    //          16      Center button in center cluster
    static Array<Variant<SDL_GamepadButton, SDL_GamepadAxis, Empty>, 17> standard_gamepad_layout {
        SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
        SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
        SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_GUIDE,
    };

    auto& realm = this->realm();

    // 1. Let inputCount be the number of button inputs exposed by the device represented by gamepad.
    // 2. Set gamepad.[[buttonMinimums]] to be a list of unsigned long values with size equal to inputCount containing minimum logical values for each of the button inputs.
    // 3. Set gamepad.[[buttonMaximums]] to be a list of unsigned long values with size equal to inputCount containing maximum logical values for each of the button inputs.
    for (auto const& standard_gamepad_button : standard_gamepad_layout) {
        standard_gamepad_button.visit(
            [this](SDL_GamepadButton button) {
                if (SDL_GamepadHasButton(m_sdl_gamepad, button)) {
                    // Buttons are binary inputs with SDL.
                    m_button_minimums.append(0);
                    m_button_maximums.append(1);
                } else {
                    // This is not a standard layout gamepad.
                    m_mapping = Bindings::GamepadMappingType::Empty;
                }
            },
            [this](SDL_GamepadAxis axis) {
                if (SDL_GamepadHasAxis(m_sdl_gamepad, axis)) {
                    // "Trigger axis values range from 0 (released) to SDL_JOYSTICK_AXIS_MAX (fully
                    // pressed) when reported by SDL_GetGamepadAxis(). Note that this is not the
                    // same range that will be reported by the lower-level SDL_GetJoystickAxis()."
                    m_button_minimums.append(0);
                    m_button_maximums.append(SDL_JOYSTICK_AXIS_MAX);
                } else {
                    // This is not a standard layout gamepad.
                    m_mapping = Bindings::GamepadMappingType::Empty;
                }
            },
            [](Empty) {
                VERIFY_NOT_REACHED();
            }
        );
    }

    // FIXME: Support non-standard gamepad buttons.
    // 4. Let unmappedInputList be an empty list.

    // FIXME: Because we don't support non-standard gamepad buttons, this would go unused.
    // 5. Let mappedIndexList be an empty list.

    // 6. Let buttonsSize be 0.
    size_t buttons_size = 0;

    // FIXME: Support non-standard gamepad buttons.
    // 7. For each rawInputIndex of the range from 0 to inputCount − 1:
    for (size_t raw_input_index = 0; raw_input_index < standard_gamepad_layout.size(); ++raw_input_index) {
        // 1. If the gamepad button at index rawInputIndex represents a Standard Gamepad button:
        // auto const& standard_gamepad_button = standard_gamepad_layout[raw_input_index];
        //
        // bool is_standard_gamepad_button = standard_gamepad_button.visit(
        //     [this](SDL_GamepadButton button) -> bool {
        //         return SDL_GamepadHasButton(m_sdl_gamepad, button);
        //     },
        //     [this](SDL_GamepadAxis axis) -> bool {
        //         return SDL_GamepadHasAxis(m_sdl_gamepad, axis);
        //     },
        //     [](Empty) -> bool {
        //         VERIFY_NOT_REACHED();
        //     }
        // );

        // 1. Let canonicalIndex be the canonical index for the button.
        // FIXME: canonicalIndex is always the same as rawInputIndex because we don't support non-standard gamepad buttons.
        auto canonical_index = raw_input_index;

        // 2. If mappedIndexList contains canonicalIndex, then append rawInputIndex to unmappedInputList.
        // FIXME: Support duplicated standard buttons.

        // Otherwise:
        // 1. Set gamepad.[[buttonMapping]][rawInputIndex] to canonicalIndex.
        m_button_mapping.set(raw_input_index, canonical_index);

        // FIXME: 2. Append canonicalIndex to mappedIndexList.

        // 3. If canonicalIndex + 1 is greater than buttonsSize, then set buttonsSize to canonicalIndex + 1.
        if (canonical_index + 1 > buttons_size)
            buttons_size = canonical_index + 1;

        // FIXME: Otherwise, append rawInputIndex to unmappedInputList.

        // 2. Increment rawInputIndex.
    }

    // FIXME: Support non-standard gamepad buttons.
    //        8. Let buttonIndex be 0.
    //        9. For each rawInputIndex of unmappedInputList:
    //           1. While mappedIndexList contains buttonIndex:
    //              1. Increment buttonIndex.
    //           2. Set gamepad.[[buttonMapping]][rawInputIndex] to buttonIndex.
    //           3. Append buttonIndex to mappedIndexList.
    //           4. If buttonIndex + 1 is greater than buttonsSize, then set buttonsSize to buttonIndex + 1.

    // NOTE: Instead of returning a list (and thus needing to use RootVector), we can just directly update m_buttons.
    // 10. Let buttons be an empty list.
    // 11. For each buttonIndex of the range from 0 to buttonsSize − 1, append a new GamepadButton to buttons.
    // 12. Return buttons.
    for (size_t button_index = 0; button_index < buttons_size; ++button_index) {
        auto gamepad_button = realm.create<GamepadButton>(realm);
        m_buttons.append(gamepad_button);
    }
}

void Gamepad::set_exposed(Badge<NavigatorGamepadPartial>, bool value)
{
    m_exposed = value;
}

void Gamepad::set_timestamp(Badge<NavigatorGamepadPartial>, HighResolutionTime::DOMHighResTimeStamp value)
{
    m_timestamp = value;
}

// https://w3c.github.io/gamepad/#dfn-update-gamepad-state
void Gamepad::update_gamepad_state()
{
    // 1. Let now be the current high resolution time given gamepad's relevant global object.
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
    auto now = HighResolutionTime::current_high_resolution_time(window);

    // 2. Set gamepad.[[timestamp]] to now.
    m_timestamp = now;

    // FIXME: 3. Run the steps to map and normalize axes for gamepad.

    // FIXME: 4. Run the steps to map and normalize buttons for gamepad.

    // FIXME: 5. Run the steps to record touches for gamepad.

    // FIXME: 6. Let navigator be gamepad's relevant global object's Navigator object.

    // FIXME: 7. If navigator.[[hasGamepadGesture]] is false and gamepad contains a gamepad user gesture:

}


}
