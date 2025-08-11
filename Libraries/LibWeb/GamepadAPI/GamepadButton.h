/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <SDL3/SDL_gamepad.h>

namespace Web::GamepadAPI {

class GamepadButton final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GamepadButton, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GamepadButton);

public:
    virtual ~GamepadButton() override;

private:
    GamepadButton(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    SDL_GamepadButton m_sdl_button { SDL_GAMEPAD_BUTTON_INVALID };

    // https://w3c.github.io/gamepad/#dfn-pressed
    // A flag indicating that the button is pressed
    bool m_pressed { false };

    // https://w3c.github.io/gamepad/#dfn-touched
    // A flag indicating that the button is touched
    bool m_touched { false };

    // https://w3c.github.io/gamepad/#dfn-value
    // A double representing the button value scaled to the range [0 .. 1]
    double m_value { 0.0 };
};

}
