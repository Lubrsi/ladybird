/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
 
#pragma once

namespace Web::TrustedTypes {

struct TrustedTypePolicyOptions {
    GC::Root<WebIDL::CallbackType> create_html;
    GC::Root<WebIDL::CallbackType> create_script;
    GC::Root<WebIDL::CallbackType> create_script_url;
};

}
