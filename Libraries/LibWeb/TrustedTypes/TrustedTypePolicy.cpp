/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrustedTypePolicyPrototype.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicy);

TrustedTypePolicy::TrustedTypePolicy(JS::Realm& realm, Utf16String name, TrustedTypePolicyOptions options)
    : Bindings::PlatformObject(realm)
    , m_name(move(name))
    , m_options(move(options))
{
}

TrustedTypePolicy::~TrustedTypePolicy() = default;

void TrustedTypePolicy::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicy);
    Base::initialize(realm);
}

}
