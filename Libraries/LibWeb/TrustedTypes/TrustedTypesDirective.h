/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::TrustedTypes {

class TrustedTypesDirective final : public ContentSecurityPolicy::Directives::Directive {
    GC_CELL(TrustedTypesDirective, ContentSecurityPolicy::Directives::Directive)
    GC_DECLARE_ALLOCATOR(TrustedTypesDirective);

public:
    virtual ~TrustedTypesDirective() = default;

private:
    TrustedTypesDirective(String name, Vector<String> value);
};

}
