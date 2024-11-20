/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Cell.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#violation
// A violation represents an action or resource which goes against the set of policy objects associated with a global
// object.
class Violation : public JS::Cell {
    GC_CELL(Violation, JS::Cell);

public:
    enum class Resource {
        Inline,
        Eval,
        WasmEval,
        TrustedTypesPolicy,
        TrustedTypesSink,
    };

    using ResourceType = Variant<Empty, Resource, URL::URL>;

    virtual ~Violation() = default;

    static GC::Ref<Violation> create_a_violation_object_for_global_policy_and_directive(JS::Realm& realm, JS::Object& global_object, Policy const& policy, String directive);
    static GC::Ref<Violation> create_a_violation_object_for_request_and_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request, Policy const& policy);

    // https://w3c.github.io/webappsec-csp/#violation-url
    URL::URL url() const;

    u16 status() const { return m_status; }
    void set_status(u16 status) { m_status = status; }

    ResourceType const& resource() const { return m_resource; }
    void set_resource(ResourceType resource) { m_resource = resource; }

    Optional<String> referrer() const { return m_referrer; }

    Policy const& policy() const { return m_policy; }

    // https://w3c.github.io/webappsec-csp/#violation-disposition
    Policy::Disposition disposition() const { return m_policy.disposition(); }

    String const& effective_directive() const { return m_effective_directive; }

    Optional<URL::URL> source_file() const { return m_source_file; }
    void set_source_file(URL::URL source_file) { m_source_file = source_file; }

    u32 line_number() const { return m_line_number; }
    void set_line_number(u32 line_number) { m_line_number = line_number; }

    u32 column_number() const { return m_column_number; }
    void set_column_number(u32 column_number) { m_column_number = column_number; }

    GC::Ptr<DOM::Element> element() const { return m_element; }
    void set_element(GC::Ref<DOM::Element> element) { m_element = element; }

    String const& sample() const { return m_sample; }
    void set_sample(String sample) { m_sample = sample; }

    void report_a_violation();

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    Violation(JS::Object& global_object, Policy policy, String directive);

    // https://w3c.github.io/webappsec-csp/#violation-global-object
    // Each violation has a global object, which is the global object whose policy has been violated.
    GC::Ref<JS::Object> m_global_object;

    // https://w3c.github.io/webappsec-csp/#violation-status
    // Each violation has a status which is a non-negative integer representing the HTTP status code of the resource
    // for which the global object was instantiated.
    u16 m_status { 0 };

    // https://w3c.github.io/webappsec-csp/#violation-resource
    // Each violation has a resource, which is either null, "inline", "eval", "wasm-eval", "trusted-types-policy"
    // "trusted-types-sink" or a URL. It represents the resource which violated the policy.
    // Spec Note:  The value null for a violation’s resource is only allowed while the violation is being populated.
    //             By the time the violation is reported and its resource is used for obtaining the blocked URI, the
    //             violation’s resource should be populated with a URL or one of the allowed strings.
    ResourceType m_resource;

    // https://w3c.github.io/webappsec-csp/#violation-referrer
    // Each violation has a referrer, which is either null, or a URL. It represents the referrer of the resource whose
    // policy was violated.
    Optional<String> m_referrer;

    // https://w3c.github.io/webappsec-csp/#violation-policy
    // Each violation has a policy, which is the policy that has been violated.
    Policy m_policy;

    // https://w3c.github.io/webappsec-csp/#violation-effective-directive
    // Each violation has an effective directive which is a non-empty string representing the directive whose enforcement
    // caused the violation.
    String m_effective_directive;

    // https://w3c.github.io/webappsec-csp/#violation-source-file
    // Each violation has a source file, which is either null or a URL.
    Optional<URL::URL> m_source_file;

    // https://w3c.github.io/webappsec-csp/#violation-line-number
    // Each violation has a line number, which is a non-negative integer.
    u32 m_line_number { 0 };

    // https://w3c.github.io/webappsec-csp/#violation-column-number
    // Each violation has a column number, which is a non-negative integer.
    u32 m_column_number { 0 };

    // https://w3c.github.io/webappsec-csp/#violation-element
    // Each violation has a element, which is either null or an element.
    GC::Ptr<DOM::Element> m_element;

    // https://w3c.github.io/webappsec-csp/#violation-sample
    // Each violation has a sample, which is a string. It is the empty string unless otherwise specified.
    String m_sample;
};

}
