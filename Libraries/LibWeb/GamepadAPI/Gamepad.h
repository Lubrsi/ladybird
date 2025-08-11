/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <SDL3/SDL_gamepad.h>

namespace Web::GamepadAPI {

// https://w3c.github.io/gamepad/#dom-gamepad
class Gamepad final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Gamepad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Gamepad);

public:
    static GC::Ref<Gamepad> create(JS::Realm&, SDL_JoystickID);

    Utf16String const& id() const { return m_id; }

    size_t index() const { return m_index; }

    bool connected() const { return m_connected; }

    HighResolutionTime::DOMHighResTimeStamp timestamp() const { return m_timestamp; }
    void set_timestamp(Badge<NavigatorGamepadPartial>, HighResolutionTime::DOMHighResTimeStamp);

    bool exposed() const { return m_exposed; }
    void set_exposed(Badge<NavigatorGamepadPartial>, bool);

private:
    explicit Gamepad(JS::Realm&, SDL_JoystickID);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    // https://w3c.github.io/gamepad/#dom-gamepad-id
    // An identification string for the gamepad. This string identifies the brand or style of connected gamepad device.
    // The exact format of the id string is left unspecified. It is RECOMMENDED that the user agent select a string
    // that identifies the product without uniquely identifying the device. For example, a USB gamepad may be
    // identified by its idVendor and idProduct values. Unique identifiers like serial numbers or Bluetooth device
    // addresses MUST NOT be included in the id string.
    Utf16String m_id;

    // https://w3c.github.io/gamepad/#dom-gamepad-index
    // The index of the gamepad in the Navigator. When multiple gamepads are connected to a user agent, indices MUST be
    // assigned on a first-come, first-serve basis, starting at zero. If a gamepad is disconnected, previously assigned
    // indices MUST NOT be reassigned to gamepads that continue to be connected. However, if a gamepad is disconnected,
    // and subsequently the same or a different gamepad is then connected, the lowest previously used index MUST be
    // reused.
    size_t m_index { 0 };

    // https://w3c.github.io/gamepad/#dfn-connected
    // A flag indicating that the device is connected to the system
    bool m_connected { false };

    // https://w3c.github.io/gamepad/#dfn-timestamp
    // The last time data for this Gamepad was updated
    HighResolutionTime::DOMHighResTimeStamp m_timestamp { 0.0 };

    // https://w3c.github.io/gamepad/#dfn-axes
    // A sequence of double values representing the current state of axes exposed by this device
    Vector<double> m_axes;

    // https://w3c.github.io/gamepad/#dfn-exposed
    // A flag indicating that the Gamepad object has been exposed to script
    bool m_exposed { false };

    SDL_JoystickID m_sdl_joystick_id { 0 };
    SDL_Gamepad* m_sdl_gamepad { nullptr };
};

}
