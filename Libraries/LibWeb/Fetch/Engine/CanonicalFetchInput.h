/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Fetch/Engine/BodyIntent.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/FileAPI/SerializedBlobURLEntry.h>

namespace Web::Fetch::Engine {

// The engine's only start input.
struct CanonicalFetchInput {
    FetchId fetch_id;
    BodyIntent body_intent;

    // Resolved on the main agent at fetch start.
    Optional<FileAPI::SerializedBlobURLEntry> blob_url_entry;
};

}
