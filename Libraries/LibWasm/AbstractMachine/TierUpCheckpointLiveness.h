/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>

namespace Wasm {

class Expression;

ErrorOr<bool> populate_tier_up_checkpoint_live_locals(Expression const&, size_t local_count);

}
