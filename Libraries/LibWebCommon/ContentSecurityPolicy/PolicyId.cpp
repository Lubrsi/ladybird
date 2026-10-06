/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibWebCommon/ContentSecurityPolicy/PolicyId.h>

namespace Web::ContentSecurityPolicy {

PolicyId generate_a_policy_id()
{
    return PolicyId { get_random<u64>() };
}

}
