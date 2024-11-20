/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Forward.h>

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy {

bool csp_list_contains_header_delivered_policy(Vector<Policy> const& csp_list);

}
