/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedTypePolicyFactoryPrototype.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyOptions.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::TrustedTypes {

class TrustedTypePolicyFactory final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicyFactory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicyFactory);

public:
    [[nodiscard]] static GC::Ref<TrustedTypePolicyFactory> create(JS::Realm&);

    virtual ~TrustedTypePolicyFactory() override { }

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_policy(Utf16String const& name, TrustedTypePolicyOptions const& options);

    bool is_html(JS::Value value);
    bool is_script(JS::Value value);
    bool is_script_url(JS::Value value);

    GC::Ref<TrustedHTML> empty_html();
    GC::Ref<TrustedScript> empty_script();

    Optional<String> get_attribute_type(String const& tag_name, String& attribute, Optional<String> element_ns, Optional<String> attr_ns);

private:
    explicit TrustedTypePolicyFactory(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(Utf16String const& policy_name, TrustedTypePolicyOptions const& options, JS::Object& global);

    Vector<Utf16String> m_created_policy_names;
    GC::Ptr<TrustedHTML> m_empty_html;
    GC::Ptr<TrustedScript> m_empty_script;
    GC::Ptr<TrustedTypePolicy> m_default_policy;
};

struct TrustedTypeData {
    String element;
    Optional<String> attribute_ns;
    String attribute_local_name;
    String trusted_type;
    String sink;
};

Optional<TrustedTypeData> get_trusted_type_data_for_attribute(String const&, String const&, Optional<String> const&);

}
