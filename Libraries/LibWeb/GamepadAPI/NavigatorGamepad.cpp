/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/GamepadAPI/EventNames.h>
#include <LibWeb/GamepadAPI/Gamepad.h>
#include <LibWeb/GamepadAPI/GamepadEvent.h>
#include <LibWeb/GamepadAPI/NavigatorGamepad.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::GamepadAPI {

// https://w3c.github.io/gamepad/#dom-navigator-getgamepads
WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> NavigatorGamepadPartial::get_gamepads()
{
    auto& navigator = as<HTML::Navigator>(*this);
    auto& realm = navigator.realm();
    auto& heap = realm.heap();

    // 1. Let doc be the current global object's associated Document.
    auto& window = as<HTML::Window>(HTML::current_principal_global_object());
    auto& document = window.associated_document();

    // 2. If doc is null or doc is not fully active, then return an empty list.
    GC::RootVector<GC::Ptr<Gamepad>> gamepads { heap };
    if (!document.is_fully_active())
        return gamepads;

    // 3. If doc is not allowed to use the "gamepad" permission, then throw a "SecurityError" DOMException and abort these steps.
    if (!document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Gamepad))
        return WebIDL::SecurityError::create(realm, "Not allowed to use gamepads"_string);

    // FIXME: 4. If this.[[hasGamepadGesture]] is false, then return an empty list.

    // 5. Let now be the current high resolution time given the current global object.
    auto now = HighResolutionTime::current_high_resolution_time(window);

    // 6. Let gamepads be an empty list.
    // NOTE: Already done.

    // 7. For each gamepad of this.[[gamepads]]:
    for (auto gamepad : m_gamepads)
    {
        // 1. If gamepad is not null and gamepad.[[exposed]] is false:
        if (gamepad && gamepad->exposed()) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed({}, true);

            // 2. Set gamepad.[[timestamp]] to now.
            gamepad->set_timestamp({}, now);
        }

        // 2. Append gamepad to gamepads.
        gamepads.append(gamepad);
    }

    // 8. Return gamepads.
    return gamepads;
}

void NavigatorGamepadPartial::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_gamepads);
}

// https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
size_t NavigatorGamepadPartial::select_an_unused_gamepad_index(Badge<Gamepad>)
{
    // 2. Let maxGamepadIndex be the size of navigator.[[gamepads]] − 1.
    // 3. For each gamepadIndex of the range from 0 to maxGamepadIndex:
    for (size_t gamepad_index = 0; gamepad_index < m_gamepads.size(); ++gamepad_index) {
        // 1. If navigator.[[gamepads]][gamepadIndex] is null, then return gamepadIndex.
        if (!m_gamepads[gamepad_index])
            return gamepad_index;
    }

    // 4. Append null to navigator.[[gamepads]].
    m_gamepads.append(nullptr);

    // 5. Return the size of navigator.[[gamepads]] − 1.
    return m_gamepads.size() - 1;
}

// https://w3c.github.io/gamepad/#event-gamepadconnected
void NavigatorGamepadPartial::handle_gamepad_connected(Badge<EventHandler>, SDL_JoystickID sdl_joystick_id)
{
    // 1. Let document be the current global object's associated Document; otherwise null.
    // FIXME: We can't use the current global object here, since it's not executing in a scripting context.
    // NOTE: NavigatorGamepad is only available on Window.
    // NOTE: document is never null.
    auto& navigator = as<HTML::Navigator>(*this);
    auto& realm = navigator.realm();
    auto& window = as<HTML::Window>(HTML::relevant_global_object(navigator));
    auto& document = window.associated_document();

    // 2. If document is not null and is not allowed to use the "gamepad" permission, then abort these steps.
    if (!document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Gamepad))
        return;

    // 3. Queue a global task on the gamepad task source with the current global object to perform the following steps:
    HTML::queue_global_task(HTML::Task::Source::Gamepad, window, GC::create_function(realm.heap(), [&realm, &document, sdl_joystick_id] mutable {
        // 1. Let gamepad be a new Gamepad representing the gamepad.
        auto gamepad = Gamepad::create(realm, sdl_joystick_id);

        // 2. Let navigator be gamepad's relevant global object's Navigator object.
        auto& gamepad_window = as<HTML::Window>(HTML::relevant_global_object(gamepad));
        auto navigator = gamepad_window.navigator();

        // 3. Set navigator.[[gamepads]][gamepad.index] to gamepad.
        navigator->m_gamepads[gamepad->index()] = gamepad;

        // 4. If navigator.[[hasGamepadGesture]] is true:
        if (navigator->m_has_gamepad_gesture) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed({}, true);

            // 2. If document is not null and is fully active, then fire an event named gamepadconnected at gamepad's
            //    relevant global object using GamepadEvent with its gamepad attribute initialized to gamepad.
            if (document.is_fully_active()) {
                auto gamepad_connected_event_init = GamepadEventInit {
                    .gamepad = gamepad,
                };
                auto gamepad_connected_event = MUST(GamepadEvent::construct_impl(realm, EventNames::gamepadconnected, gamepad_connected_event_init));
                gamepad_window.dispatch_event(gamepad_connected_event);
            }
        }
    }));
}

}
