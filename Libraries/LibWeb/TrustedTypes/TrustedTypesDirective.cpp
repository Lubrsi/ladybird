/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypesDirective.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypesDirective);

TrustedTypesDirective::TrustedTypesDirective(String name, Vector<String> value)
    : Directive(move(name), move(value))
{
}

}
