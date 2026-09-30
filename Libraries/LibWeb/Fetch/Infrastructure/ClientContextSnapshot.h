/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/KeepaliveQuotaAccountant.h>
#include <LibWeb/MixedContent/ProhibitsMixedSecurityContexts.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace Web::Fetch::Infrastructure {

enum class HasCrossSiteAncestor : u8 {
    No,
    Yes,
};

// What fetching reads from a request's client, as it was when the fetch started.
struct WEB_API ClientContextSnapshot final : public AtomicRefCounted<ClientContextSnapshot> {
    enum class GlobalKind : u8 {
        Window,
        Worker,
        Other,
    };

    enum class IsSecureContext : u8 {
        No,
        Yes,
    };

    // Whether the global object is a Window whose navigable is non-null and has no parent.
    enum class IsWindowOfATopLevelNavigable : u8 {
        No,
        Yes,
    };

    ClientContextSnapshot(URL::Origin origin, NonnullRefPtr<KeepaliveQuotaAccountant> keepalive_quota_accountant)
        : origin(move(origin))
        , keepalive_quota_accountant(move(keepalive_quota_accountant))
    {
    }

    URL::Origin origin;
    URL::URL creation_url;
    Optional<URL::URL> top_level_creation_url;
    Optional<URL::Origin> top_level_origin;
    GlobalKind global_kind { GlobalKind::Other };
    IsWindowOfATopLevelNavigable is_window_of_a_top_level_navigable { IsWindowOfATopLevelNavigable::No };
    HasCrossSiteAncestor has_cross_site_ancestor { HasCrossSiteAncestor::No };
    HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability { HTML::CanUseCrossOriginIsolatedAPIs::No };
    IsSecureContext is_secure_context { IsSecureContext::No };
    MixedContent::ProhibitsMixedSecurityContexts prohibits_mixed_security_contexts { MixedContent::ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts };

    // The URL a "client" referrer is determined from, or none for no referrer.
    Optional<URL::URL> referrer_source;

    // The URL the content blocker treats as the source of a request that is not for a document.
    URL::URL content_blocker_source_url;

    NonnullRefPtr<KeepaliveQuotaAccountant> keepalive_quota_accountant;
};

// What fetching reads from a request's reserved client, as it was when the reserved client was set.
struct WEB_API ReservedClientContextSnapshot final : public AtomicRefCounted<ReservedClientContextSnapshot> {
    enum class TargetBrowsingContextIsTopLevel : u8 {
        No,
        Yes,
    };

    // The origin of a reserved client that is a settings object, and whether it has a cross-site ancestor.
    struct SettingsObject {
        URL::Origin origin;
        HasCrossSiteAncestor has_cross_site_ancestor { HasCrossSiteAncestor::No };
    };

    // The parent of the target browsing context's navigable: its active document's origin, and whether that document
    // has a cross-site ancestor.
    struct Parent {
        Optional<URL::Origin> active_document_origin;
        HasCrossSiteAncestor active_document_has_cross_site_ancestor { HasCrossSiteAncestor::No };
    };

    URL::URL creation_url;
    Optional<URL::URL> top_level_creation_url;
    Optional<URL::Origin> top_level_origin;
    TargetBrowsingContextIsTopLevel target_browsing_context_is_top_level { TargetBrowsingContextIsTopLevel::No };
    Optional<SettingsObject> settings_object;
    Optional<Parent> parent;
};

}
