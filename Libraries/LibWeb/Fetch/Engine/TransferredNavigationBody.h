/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>

namespace Web::Fetch::Engine {

// The process adopting the body reads these bytes first, then what RequestServer still has.
struct StreamContinuation {
    Vector<FetchByteChannel::TakenBytes> prefix;
};

// RequestServer delivers the whole body again to the process adopting it.
struct WholeFileReplay { };

using TransferredNavigationBody = Variant<StreamContinuation, WholeFileReplay>;

}
