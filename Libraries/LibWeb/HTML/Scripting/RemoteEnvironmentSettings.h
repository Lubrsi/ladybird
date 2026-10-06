/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace Web::HTML {

// A navigation whose request has a client another process hosts, by which the UI process finds that client again.
struct RemoteClientNavigation {
    CrossProcessId navigable_id;
    Utf16String navigation_id;
};

// The settings of an environment another process hosts, as that process serialized them.
struct RemoteEnvironmentSettings final : public AtomicRefCounted<RemoteEnvironmentSettings> {
    explicit RemoteEnvironmentSettings(SerializedEnvironmentSettingsObject settings, Optional<RemoteClientNavigation> navigation = {})
        : settings(move(settings))
        , navigation(move(navigation))
    {
    }

    SerializedEnvironmentSettingsObject const settings;
    Optional<RemoteClientNavigation> const navigation;
};

}
