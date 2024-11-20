/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

Optional<StringView> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request);

}
