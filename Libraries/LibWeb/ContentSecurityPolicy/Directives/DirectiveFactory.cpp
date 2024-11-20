/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/DefaultSource.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>

namespace Web::ContentSecurityPolicy::Directives {

Directive create_directive(String name, Vector<String> value)
{
    if (name == "default-src"sv)
        return DefaultSource(move(name), move(value));

    dbgln("Potential FIXME: Creating unknown Content Security Policy directive: {}", name);
    return Directive(move(name), move(value));
}

}
