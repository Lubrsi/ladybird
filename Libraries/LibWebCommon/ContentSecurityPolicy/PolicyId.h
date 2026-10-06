/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <LibWebCommon/Export.h>

namespace Web::ContentSecurityPolicy {

// A policy's identity, which every copy of the policy keeps, in this process or another.
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, PolicyId);

WEBCOMMON_API PolicyId generate_a_policy_id();

}
