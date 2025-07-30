/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/WindowEventHandlers.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicyFactory);

GC::Ref<TrustedTypePolicyFactory> TrustedTypePolicyFactory::create(JS::Realm& realm)
{
    return realm.create<TrustedTypePolicyFactory>(realm);
}

// https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-createpolicy
WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> TrustedTypePolicyFactory::create_policy(Utf16String const& name, TrustedTypePolicyOptions const& options)
{
    // Returns the result of executing a Create a Trusted Type Policy algorithm, with the following arguments:
    // factory
    //      this value
    // policyName
    //      policyName
    // options
    //      policyOptions
    // global
    //      this value’s relevant global object
    return create_a_trusted_type_policy(name, options, HTML::relevant_global_object(*this));
}

// https://www.w3.org/TR/trusted-types/#abstract-opdef-should-trusted-type-policy-creation-be-blocked-by-content-security-policy
static ContentSecurityPolicy::Directives::Directive::Result should_trusted_type_policy_creation_be_blocked_by_content_security_policy(JS::Realm& realm, JS::Object& global, Utf16String const& policy_name, Vector<Utf16String> const& created_policy_names)
{
    // 1. Let result be "Allowed".
    auto result = ContentSecurityPolicy::Directives::Directive::Result::Allowed;

    // 2. For each policy in global’s CSP list:
    auto csp_list = ContentSecurityPolicy::PolicyList::from_object(global);
    VERIFY(csp_list);
    for (auto const policy : csp_list->policies()) {
        // 1. Let createViolation be false.
        bool create_violation = false;

        // 2. If policy’s directive set does not contain a directive which name is "trusted-types", skip to the next
        //    policy.
        auto maybe_trusted_types = policy->directives().find_if([](auto const& directive) {
            return directive->name() == ContentSecurityPolicy::Directives::Names::TrustedTypes;
        });

        if (maybe_trusted_types.is_end())
            continue;

        // 3. Let directive be the policy’s directive set’s directive which name is "trusted-types"
        auto trusted_types_directive = *maybe_trusted_types;

        // 4. If directive’s value only contains a tt-keyword which is a match for a value 'none', set createViolation
        //    to true.
        // Spec Note: Like in other CSP directives, 'none' keyword will be ignored if other keywords or policy names
        //            are present.
        // https://www.w3.org/TR/trusted-types/#tt-keyword
        // tt-keyword = "'allow-duplicates'" / "'none'"
        if (trusted_types_directive->value().size() == 1 && trusted_types_directive->value().first().equals_ignoring_ascii_case("'none'"sv)) {
            create_violation = true;
        }

        // 5. If createdPolicyNames contains policyName and directive’s value does not contain a tt-keyword which is a
        //    match for a value 'allow-duplicates', set createViolation to true.
        // Spec Note: trusted-types policyA policyB 'allow-duplicates' allows authors to create policies with
        //            duplicated names.
        if (created_policy_names.contains_slow(policy_name)) {
            auto maybe_allow_duplicates = trusted_types_directive->value().find_if([](auto const& directive_value) {
                return directive_value.equals_ignoring_ascii_case("'allow-duplicates'"sv);
            });

            if (maybe_allow_duplicates.is_end()) {
                create_violation = true;
            }
        }

        // FIXME: 6. If directive’s value does not contain a tt-policy-name, which value is policyName, and directive’s
        //           value does not contain a tt-wildcard, set createViolation to true.

        // 7. If createViolation is false, skip to the next policy.
        if (!create_violation)
            continue;

        // 8. Let violation be the result of executing Create a violation object for global, policy, and directive on
        //    global, policy and "trusted-types"
        auto violation = ContentSecurityPolicy::Violation::create_a_violation_object_for_global_policy_and_directive(realm, global, policy, ContentSecurityPolicy::Directives::Names::TrustedTypes.to_string());

        // 9. Set violation’s resource to "trusted-types-policy".
        violation->set_resource(ContentSecurityPolicy::Violation::Resource::TrustedTypesPolicy);

        // 10. Set violation’s sample to the substring of policyName, containing its first 40 characters.
        auto name_sample = policy_name.substring_view(0, 40).to_utf8_but_should_be_ported_to_utf16();
        violation->set_sample(move(name_sample));

        // 11. Execute Report a violation on violation.
        violation->report_a_violation(realm);

        // 12. If policy’s disposition is "enforce", then set result to "Blocked".
        if (policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce)
            result = ContentSecurityPolicy::Directives::Directive::Result::Blocked;
    }

    // 3. Return result.
    return result;
}

// https://www.w3.org/TR/trusted-types/#abstract-opdef-create-a-trusted-type-policy
WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> TrustedTypePolicyFactory::create_a_trusted_type_policy(Utf16String const& policy_name, TrustedTypePolicyOptions const& options, JS::Object& global)
{
    auto& realm = this->realm();

    // 1. Let allowedByCSP be the result of executing Should Trusted Type policy creation be blocked by Content
    //    Security Policy? algorithm with global, policyName and factory’s created policy names value.
    auto allowed_by_csp = should_trusted_type_policy_creation_be_blocked_by_content_security_policy(realm, global, policy_name, m_created_policy_names);

    // 2. If allowedByCSP is "Blocked", throw a TypeError and abort further steps.
    if (allowed_by_csp == ContentSecurityPolicy::Directives::Directive::Result::Blocked)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Blocked by Content Security Policy"sv };

    // 3. If policyName is default and the factory’s default policy value is not null, throw a TypeError and abort
    //    further steps.
    if (m_default_policy && policy_name == "default"sv)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "A default policy has already been defined"sv };

    // 4. Let policy be a new TrustedTypePolicy object.
    // 5. Set policy’s name property value to policyName.
    // 6. Set policy’s options value to «[ "createHTML" -> options["createHTML", "createScript" -> options["createScript", "createScriptURL" -> options["createScriptURL" ]».
    auto policy = realm.create<TrustedTypePolicy>(realm, policy_name, options);

    // 7. If the policyName is default, set the factory’s default policy value to policy.
    if (policy_name == "default"sv)
        m_default_policy = policy;

    // 8. Append policyName to factory’s created policy names.
    m_created_policy_names.append(policy_name);

    // 9. Return policy.
    return policy;
}

// https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-ishtml
bool TrustedTypePolicyFactory::is_html(JS::Value value)
{
    // Returns true if value is an instance of TrustedHTML and has an associated data value set, false otherwise.
    if (!value.is_object())
        return false;

    return is<TrustedHTML>(value.as_object());
}

// https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-isscript
bool TrustedTypePolicyFactory::is_script(JS::Value value)
{
    // Returns true if value is an instance of TrustedScript and has an associated data value set, false otherwise.
    if (!value.is_object())
        return false;

    return is<TrustedScript>(value.as_object());
}

// https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-isscripturl
bool TrustedTypePolicyFactory::is_script_url(JS::Value value)
{
    // Returns true if value is an instance of TrustedScriptURL and has an associated data value set, false otherwise.
    if (!value.is_object())
        return false;

    return is<TrustedScriptURL>(value.as_object());
}

// https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-emptyhtml
GC::Ref<TrustedHTML> TrustedTypePolicyFactory::empty_html()
{
    auto& realm = this->realm();

    // emptyHTML, of type TrustedHTML, readonly
    //      is a TrustedHTML object with its data value set to an empty string.
    if (!m_empty_html)
        m_empty_html = realm.create<TrustedHTML>(realm, ""_utf16);

    return *m_empty_html;
}

// https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-emptyhtml
GC::Ref<TrustedScript> TrustedTypePolicyFactory::empty_script()
{
    auto& realm = this->realm();

    // emptyScript, of type TrustedScript, readonly
    //      is a TrustedScript object with its data value set to an empty string.
    if (!m_empty_script)
        m_empty_script = realm.create<TrustedScript>(realm, ""_utf16);

    return *m_empty_script;
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getattributetype
Optional<String> TrustedTypePolicyFactory::get_attribute_type(String const& tag_name, String& attribute, Optional<String> element_ns, Optional<String> attr_ns)
{
    // 1. Set localName to tagName in ASCII lowercase.
    auto const local_name = tag_name.to_ascii_lowercase();

    // 2. Set attribute to attribute in ASCII lowercase.
    attribute = attribute.to_ascii_lowercase();

    // 3. If elementNs is null or an empty string, set elementNs to HTML namespace.
    if (!element_ns.has_value() || element_ns.value().is_empty())
        element_ns = String { Namespace::HTML };

    // 4. If attrNs is an empty string, set attrNs to null.
    if (attr_ns.has_value() && attr_ns.value().is_empty())
        attr_ns.clear();

    // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
    // we care about in the table in get_trusted_type_data_for_attribute function
    // 5. Let interface be the element interface for localName and elementNs.
    String interface;
    if (local_name == HTML::TagNames::iframe && element_ns == Namespace::HTML) {
        interface = "HTMLIFrameElement"_string;
    } else if (local_name == HTML::TagNames::script && element_ns == Namespace::HTML) {
        interface = "HTMLScriptElement"_string;
    } else if (local_name == SVG::TagNames::script && element_ns == Namespace::SVG) {
        interface = "SVGScriptElement"_string;
    } else {
        interface = "Element"_string;
    }

    // 6. Let expectedType be null.
    Optional<String> expected_type {};

    // 7. Set attributeData to the result of Get Trusted Type data for attribute algorithm,
    // with the following arguments, interface as element, attribute, attrNs
    auto const attribute_data = get_trusted_type_data_for_attribute(interface, attribute, attr_ns);

    // 8. If attributeData is not null, then set expectedType to the interface’s name of the value of the fourth member of attributeData.
    if (attribute_data.has_value()) {
        expected_type = attribute_data.value().trusted_type;
    }

    // 9. Return expectedType.
    return expected_type;
}

TrustedTypePolicyFactory::TrustedTypePolicyFactory(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void TrustedTypePolicyFactory::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicyFactory);
    Base::initialize(realm);
}

void TrustedTypePolicyFactory::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_empty_html);
    visitor.visit(m_empty_script);
    visitor.visit(m_default_policy);
}

// https://w3c.github.io/trusted-types/dist/spec/#abstract-opdef-get-trusted-type-data-for-attribute
Optional<TrustedTypeData> get_trusted_type_data_for_attribute(String const& element, String const& attribute, Optional<String> const& attribute_ns)
{
    // 1. Let data be null.
    Optional<TrustedTypeData const&> data {};

    // 2. If attributeNs is null, and attribute is the name of an event handler content attribute, then:
    if (!attribute_ns.has_value()) {
#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                                                                   \
    if (attribute == HTML::AttributeNames::attribute_name) {                                                                      \
        /* 1. Return (Element, null, attribute, TrustedScript, "Element " + attribute). */                                        \
        return TrustedTypeData { "Element"_string, {}, attribute, "TrustedScript"_string, "Element " #attribute_name ""_string }; \
    }
        ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
        ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
    }

    static Vector<TrustedTypeData> const table {
        { "HTMLIFrameElement"_string, {}, "srcdoc"_string, "TrustedHTML"_string, "HTMLIFrameElement srcdoc"_string },
        { "HTMLScriptElement"_string, {}, "src"_string, "TrustedScriptURL"_string, "HTMLScriptElement src"_string },
        { "SVGScriptElement"_string, {}, "href"_string, "TrustedScriptURL"_string, "SVGScriptElement href"_string },
        { "SVGScriptElement"_string, Namespace::XLink.to_string(), "href"_string, "TrustedScriptURL"_string, "SVGScriptElement href"_string },
    };

    // 3. Find the row in the following table, where element is in the first column, attributeNs is in the second column,
    // and attribute is in the third column. If a matching row is found, set data to that row.
    data = table.first_matching([&element, &attribute, &attribute_ns](auto const& row) {
        return row.element == element && row.attribute_ns == attribute_ns && row.attribute_local_name == attribute;
    });

    // 4. Return data
    return data.copy();
}

}
