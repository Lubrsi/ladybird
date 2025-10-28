/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fullscreen/EventNames.h>

namespace Web::Fullscreen::EventNames {

#define __ENUMERATE_FULLSCREEN_EVENT(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_FULLSCREEN_EVENTS
#undef __ENUMERATE_FULLSCREEN_EVENT

}
