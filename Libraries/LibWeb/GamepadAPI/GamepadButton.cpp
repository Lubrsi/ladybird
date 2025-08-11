/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/GamepadButtonPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/GamepadAPI/GamepadButton.h>

namespace Web::GamepadAPI {

GC_DEFINE_ALLOCATOR(GamepadButton);

GamepadButton::GamepadButton(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

GamepadButton::~GamepadButton() = default;

void GamepadButton::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GamepadButton);
    Base::initialize(realm);
}

}
