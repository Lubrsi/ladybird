/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>

namespace Web::ContentSecurityPolicy::Directives {

class DefaultSource final : public Directive {
public:
    DefaultSource(String name, Vector<String> value);
    virtual ~DefaultSource() override = default;

    [[nodiscard]] virtual Result pre_request_check(GC::Ref<Fetch::Infrastructure::Request const>, Policy const&) const override;
    [[nodiscard]] virtual Result post_request_check(GC::Ref<Fetch::Infrastructure::Request const>, GC::Ref<Fetch::Infrastructure::Response const>, Policy const&) const override;
    [[nodiscard]] virtual Result inline_check(GC::Ref<DOM::Element const>, String const&, Policy const&, String const&) const override;
};

}
