/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

enum class ShouldExecute {
    No,
    Yes,
};

Optional<StringView> get_the_effective_directive_for_request(GC::Ref<Fetch::Infrastructure::Request const> request);
Vector<StringView> get_fetch_directive_fallback_list(Optional<StringView> directive_name);
ShouldExecute should_fetch_directive_execute(Optional<StringView> effective_directive_name, StringView directive_name, Policy const& policy);

}
