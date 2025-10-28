/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

#include <AK/FlyString.h>

namespace Web::Fullscreen::EventNames {

#define ENUMERATE_FULLSCREEN_EVENTS                \
    __ENUMERATE_FULLSCREEN_EVENT(fullscreenchange)

#define __ENUMERATE_FULLSCREEN_EVENT(name) extern FlyString name;
ENUMERATE_FULLSCREEN_EVENTS
#undef __ENUMERATE_FULLSCREEN_EVENT

}
