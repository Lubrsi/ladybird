/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GamepadPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/GamepadAPI/Gamepad.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

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

void Gamepad::finalize()
{
    SDL_CloseGamepad(m_sdl_gamepad);
}

void Gamepad::set_exposed(Badge<NavigatorGamepadPartial>, bool value)
{
    m_exposed = value;
}

void Gamepad::set_timestamp(Badge<NavigatorGamepadPartial>, HighResolutionTime::DOMHighResTimeStamp value)
{
    m_timestamp = value;
}

}
