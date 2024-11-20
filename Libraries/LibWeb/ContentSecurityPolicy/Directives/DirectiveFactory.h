/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy::Directives {

Directive create_directive(String name, Vector<String> value);

}
