/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include <SDL3/sdl_gamepad.h>

namespace Web::GamepadAPI {

class NavigatorGamepadPartial {
public:
    WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> get_gamepads();

    size_t select_an_unused_gamepad_index(Badge<Gamepad>);

    void handle_gamepad_connected(Badge<EventHandler>, SDL_JoystickID sdl_joystick_id);
    void handle_gamepad_updated(Badge<EventHandler>, SDL_JoystickID sdl_joystick_id);
    void handle_gamepad_disconnected(Badge<EventHandler>, SDL_JoystickID sdl_joystick_id);

protected:
    void visit_edges(GC::Cell::Visitor& visitor);

private:
    virtual ~NavigatorGamepadPartial() = default;

    friend class HTML::Navigator;

    // https://w3c.github.io/gamepad/#dfn-hasgamepadgesture
    // A flag indicating that a gamepad user gesture has been observed
    bool m_has_gamepad_gesture { false };

    // https://w3c.github.io/gamepad/#dfn-gamepads
    // Each Gamepad present at the index specified by its index attribute, or null for unassigned indices.
    Vector<GC::Ptr<Gamepad>> m_gamepads;
};

}
