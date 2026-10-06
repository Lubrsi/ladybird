/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::ContentSecurityPolicy {

// The effective directive of a violation a fetch found of a policy of a client another process hosts.
enum class RemoteViolationDirective : u8 {
    FrameSrc,
    ObjectSrc,
    FormAction,
    WorkerSrc,
};

}
