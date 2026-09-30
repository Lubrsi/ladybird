/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/EnvironmentSettingsSnapshot.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/MixedContent/AbstractOperations.h>

namespace Web::Fetch::Fetching {

// https://w3c.github.io/webappsec-referrer-policy/#determine-requests-referrer
// NB: The steps of the "client" case that read environment, which is non-null.
static Optional<URL::URL> referrer_source_of_client(HTML::EnvironmentSettingsObject& environment)
{
    // NB: A snapshot of an environment in another process — a navigation's fetch client, once the navigation
    //     continues in the process hosting its target — has a global object from this process, so it answers
    //     from the global object it was taken from.
    if (auto const* snapshot = as_if<HTML::EnvironmentSettingsSnapshot>(environment)) {
        if (auto const* window = snapshot->serialized_global().get_pointer<HTML::SerializedWindow>()) {
            if (snapshot->origin().is_opaque())
                return {};
            return window->associated_document.url;
        }
        return environment.creation_url;
    }

    // 2. If environment’s global object is a Window object, then
    if (auto const* window = HTML::window_from_global_object(environment.global_object())) {
        // 1. Let document be the associated Document of environment’s global object.
        auto const& document = window->associated_document();

        // 2. If document’s origin is an opaque origin, return no referrer.
        if (document.origin().is_opaque())
            return {};

        // FIXME: 3. While document is an iframe srcdoc document, let document be document’s browsing context’s
        //           browsing context container’s node document.

        // 4. Let referrerSource be document’s URL.
        return document.url();
    }

    // 3. Otherwise, let referrerSource be environment’s creation URL.
    return environment.creation_url;
}

NonnullRefPtr<Infrastructure::ClientContextSnapshot const> snapshot_client_context(HTML::EnvironmentSettingsObject& client)
{
    using Snapshot = Infrastructure::ClientContextSnapshot;

    auto snapshot = make_ref_counted<Snapshot>(client.origin(), client.keepalive_quota_accountant());
    snapshot->creation_url = client.creation_url;
    snapshot->top_level_creation_url = client.top_level_creation_url;
    snapshot->top_level_origin = client.top_level_origin;

    auto& global = client.global_object();
    if (auto const* window = HTML::window_from_global_object(global)) {
        snapshot->global_kind = Snapshot::GlobalKind::Window;
        if (window->navigable() && !window->navigable()->parent())
            snapshot->is_window_of_a_top_level_navigable = Snapshot::IsWindowOfATopLevelNavigable::Yes;
    } else if (Bindings::worker_global_scope_from_global_object(global)) {
        snapshot->global_kind = Snapshot::GlobalKind::Worker;
    }

    if (client.has_cross_site_ancestor())
        snapshot->has_cross_site_ancestor = Infrastructure::HasCrossSiteAncestor::Yes;
    snapshot->cross_origin_isolated_capability = client.cross_origin_isolated_capability();
    if (HTML::is_secure_context(client))
        snapshot->is_secure_context = Snapshot::IsSecureContext::Yes;
    snapshot->prohibits_mixed_security_contexts = MixedContent::does_settings_prohibit_mixed_security_contexts(client);
    snapshot->referrer_source = referrer_source_of_client(client);

    if (auto document = client.responsible_document())
        snapshot->content_blocker_source_url = document->fallback_base_url();
    else
        snapshot->content_blocker_source_url = client.api_base_url();

    return snapshot;
}

NonnullRefPtr<Infrastructure::ReservedClientContextSnapshot const> snapshot_reserved_client_context(HTML::Environment const& reserved_client)
{
    using Snapshot = Infrastructure::ReservedClientContextSnapshot;

    auto snapshot = make_ref_counted<Snapshot>();
    snapshot->creation_url = reserved_client.creation_url;
    snapshot->top_level_creation_url = reserved_client.top_level_creation_url;
    snapshot->top_level_origin = reserved_client.top_level_origin;
    if (reserved_client.target_browsing_context && reserved_client.target_browsing_context->is_top_level())
        snapshot->target_browsing_context_is_top_level = Snapshot::TargetBrowsingContextIsTopLevel::Yes;

    if (auto const* settings_object = as_if<HTML::EnvironmentSettingsObject>(reserved_client)) {
        snapshot->settings_object = Snapshot::SettingsObject {
            .origin = settings_object->origin(),
            .has_cross_site_ancestor = settings_object->has_cross_site_ancestor() ? Infrastructure::HasCrossSiteAncestor::Yes : Infrastructure::HasCrossSiteAncestor::No,
        };
    }

    if (reserved_client.target_browsing_context) {
        auto const* document = reserved_client.target_browsing_context->active_document();
        auto navigable = document ? document->navigable() : nullptr;
        if (auto parent = navigable ? navigable->parent() : nullptr) {
            snapshot->parent = Snapshot::Parent {
                .active_document_origin = parent->active_document_origin(),
                .active_document_has_cross_site_ancestor = parent->active_document_has_cross_site_ancestor() ? Infrastructure::HasCrossSiteAncestor::Yes : Infrastructure::HasCrossSiteAncestor::No,
            };
        }
    }

    return snapshot;
}

}
