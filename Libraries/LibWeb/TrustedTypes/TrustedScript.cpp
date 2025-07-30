/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrustedScriptPrototype.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedScript);

TrustedScript::TrustedScript(JS::Realm& realm, Utf16String data)
    : Bindings::PlatformObject(realm)
    , m_data(move(data))
{
}

TrustedScript::~TrustedScript() = default;

void TrustedScript::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedScript);
    Base::initialize(realm);
}

// https://www.w3.org/TR/trusted-types/#trustedscript-stringification-behavior
Utf16String const& TrustedScript::to_string()
{
    // toJSON() method steps and the stringification behavior steps of a TrustedScript object are to return the
    // associated data value.
    return m_data;
}

// https://www.w3.org/TR/trusted-types/#dom-trustedscript-tojson
Utf16String const& TrustedScript::to_json()
{
    // toJSON() method steps and the stringification behavior steps of a TrustedScript object are to return the
    // associated data value.
    return m_data;
}

}
