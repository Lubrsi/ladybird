/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyOptions.h>

namespace Web::TrustedTypes {

class TrustedTypePolicy final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicy, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicy);

public:
    virtual ~TrustedTypePolicy() override;

    Utf16String const& name() { return m_name; }

private:
    TrustedTypePolicy(JS::Realm&, Utf16String name, TrustedTypePolicyOptions options);
    virtual void initialize(JS::Realm&) override;

    // https://www.w3.org/TR/trusted-types/#trustedtypepolicy-name
    // Each policy has a name.
    Utf16String m_name;

    // Each TrustedTypePolicy object has an associated TrustedTypePolicyOptions options object, describing the actual
    // behavior of the policy.
    TrustedTypePolicyOptions m_options;
};

}
