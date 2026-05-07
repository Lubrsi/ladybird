/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

// Optional pre-step that runs before the cascade-time SELF read inside compute_value_of_custom_property. The
// default-constructed value installs no override; the named factories install one of the two mutually-exclusive
// override modes whose details stay private to this type.
class CustomPropertyResolutionContext {
public:
    CustomPropertyResolutionContext() = default;

    // Computed-value lookup: var(name) returns the already-computed value of `name` from this ComputedProperties
    // when present (animation tick = animated values; longhand keyframe pass = scratch ComputedProperties holding
    // the just-computed keyframe custom values). Otherwise falls through to SELF cascade-time data.
    static CustomPropertyResolutionContext for_computed_custom_property_lookup(CSS::ComputedProperties const&);

    // Keyframe specified-value lookup: var(name) consults the keyframe-specified-value map. The returned value is
    // possibly unresolved; the caller (compute_value_of_custom_property) recursively computes it via
    // compute_custom_property_from_specified_value with the shared GuardedSubstitutionContexts so cycles between
    // keyframe properties get marked invalid per css-variables.
    static CustomPropertyResolutionContext for_keyframe_specified_lookup(HashMap<FlyString, RefPtr<CSS::StyleValue const>> const&);

    struct LookupResult {
        NonnullRefPtr<CSS::StyleValue const> value;
        // True when the value is a keyframe-specified value and the caller must recursively compute it. False when
        // the value is already a computed value and should be used as-is.
        bool needs_recursive_computation { false };
    };
    Optional<LookupResult> lookup_override(FlyString const& name) const;

private:
    struct ComputedValues {
        CSS::ComputedProperties const* computed_properties;
    };
    struct KeyframeSpecifiedValues {
        HashMap<FlyString, RefPtr<CSS::StyleValue const>> const* specified_values;
    };

    explicit CustomPropertyResolutionContext(ComputedValues state)
        : m_state(state)
    {
    }
    explicit CustomPropertyResolutionContext(KeyframeSpecifiedValues state)
        : m_state(state)
    {
    }

    Variant<Empty, ComputedValues, KeyframeSpecifiedValues> m_state;
};

// https://drafts.csswg.org/css-values-5/#substitution-context
struct SubstitutionContext {
    enum class DependencyType : u8 {
        Property,
        Attribute,
        Function,
    };
    DependencyType dependency_type;
    String first;
    Optional<String> second {};

    bool is_cyclic { false };

    bool operator==(SubstitutionContext const&) const;
    String to_string() const;
};

class GuardedSubstitutionContexts {
public:
    void guard(SubstitutionContext&);
    void unguard(SubstitutionContext const&);

private:
    Vector<SubstitutionContext&> m_contexts;
};

enum class ArbitrarySubstitutionFunction : u8 {
    Attr,
    Env,
    If,
    Inherit,
    Var,
};
[[nodiscard]] Optional<ArbitrarySubstitutionFunction> to_arbitrary_substitution_function(FlyString const& name);

bool contains_guaranteed_invalid_value(ReadonlySpan<ComponentValue>);

[[nodiscard]] Vector<ComponentValue> substitute_arbitrary_substitution_functions(DOM::AbstractElement&, GuardedSubstitutionContexts&, ReadonlySpan<ComponentValue>, Optional<SubstitutionContext> = {}, CustomPropertyResolutionContext const& resolution_context = {});

using DeclarationValueList = Vector<ReadonlySpan<ComponentValue>>;

struct IfArgsBranch {
    ReadonlySpan<ComponentValue> condition;
    Optional<ReadonlySpan<ComponentValue>> value;
};

using IfArgs = Vector<IfArgsBranch>;
using ArbitrarySubstitutionFunctionArguments = Variant<DeclarationValueList, IfArgs>;
// The returned argument spans borrow from the input component value list.
[[nodiscard]] Optional<ArbitrarySubstitutionFunctionArguments> parse_according_to_argument_grammar(ArbitrarySubstitutionFunction, ReadonlySpan<ComponentValue>);

[[nodiscard]] Vector<ComponentValue> replace_an_arbitrary_substitution_function(DOM::AbstractElement&, GuardedSubstitutionContexts&, ArbitrarySubstitutionFunction, ArbitrarySubstitutionFunctionArguments const&, CustomPropertyResolutionContext const& resolution_context = {});

}
