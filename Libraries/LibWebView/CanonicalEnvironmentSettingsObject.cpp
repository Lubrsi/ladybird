/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalWindow.h>

namespace WebView {

bool CanonicalEnvironmentSettingsObject::may_use_cookies_of(URL::URL const& url) const
{
    return !origin().is_opaque() && url.origin().is_same_origin(origin());
}

void CanonicalEnvironmentSettingsObject::serialize_into(Web::HTML::SerializedEnvironmentSettingsObject& settings) const
{
    VERIFY(settings.id == id());
    settings.top_level_creation_url = top_level_creation_url();
    settings.top_level_origin = top_level_origin();
    settings.origin = origin();
    settings.has_cross_site_ancestor = has_cross_site_ancestor();
    settings.cross_origin_isolated_capability = cross_origin_isolated_capability();
    settings.agent_cluster_id = agent_cluster_id();
}

CanonicalWindowEnvironmentSettingsObject::CanonicalWindowEnvironmentSettingsObject(CanonicalWindow& window, Web::HTML::EnvironmentId id, URL::URL top_level_creation_url, URL::Origin top_level_origin, RefPtr<CanonicalDocument const> container_document)
    : CanonicalEnvironmentSettingsObject(move(id), move(top_level_creation_url), move(top_level_origin))
    , m_window(window)
    , m_container_document(move(container_document))
{
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects:concept-settings-object-origin
URL::Origin const& CanonicalWindowEnvironmentSettingsObject::origin() const
{
    // Return the origin of window's associated Document.
    return m_window.associated_document().origin();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects:concept-settings-object-has-cross-site-ancestor
bool CanonicalWindowEnvironmentSettingsObject::has_cross_site_ancestor() const
{
    // 1. If window's navigable's parent is null, then return false.
    if (!m_container_document)
        return false;

    // 2. Let parentDocument be window's navigable's parent's active document.
    // NB: That is the navigable's container document while window's associated Document is fully active.
    auto const& parent_document = *m_container_document;

    // 3. If parentDocument's relevant settings object's has cross-site ancestor is true, then return true.
    if (parent_document.relevant_global_object().relevant_settings_object().has_cross_site_ancestor())
        return true;

    // 4. If parentDocument's origin is not same site with window's associated Document's origin, then return true.
    if (!parent_document.origin().is_same_site(m_window.associated_document().origin()))
        return true;

    // 5. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects:concept-settings-object-cross-origin-isolated-capability
Web::HTML::CanUseCrossOriginIsolatedAPIs CanonicalWindowEnvironmentSettingsObject::cross_origin_isolated_capability() const
{
    // Return true if both of the following hold, and false otherwise:
    // - realm's agent cluster's cross-origin-isolation mode is "concrete", and
    // - window's associated Document is allowed to use the "cross-origin-isolated" feature.
    if (m_window.agent().agent_cluster_cross_origin_isolation_mode() == CrossOriginIsolationMode::Concrete
        && associated_document_is_allowed_to_use_the_cross_origin_isolated_feature())
        return Web::HTML::CanUseCrossOriginIsolatedAPIs::Yes;
    return Web::HTML::CanUseCrossOriginIsolatedAPIs::No;
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#allowed-to-use
bool CanonicalWindowEnvironmentSettingsObject::associated_document_is_allowed_to_use_the_cross_origin_isolated_feature() const
{
    auto const& document = m_window.associated_document();

    // 1. If document's browsing context is null, then return false.
    // NB: A document the UI process holds always has a browsing context.

    // FIXME: 2. If document is not fully active, then return false.

    // 3. If the result of running is feature enabled in document for origin on feature, document, and document's
    //    origin is "Enabled", then return true.
    // AD-HOC: The feature's default allowlist of 'self' enables it in a document whose ancestor documents are all same
    //         origin with it.
    auto is_enabled = [&] {
        for (auto const* ancestor = m_container_document.ptr(); ancestor; ancestor = ancestor->relevant_global_object().relevant_settings_object().container_document()) {
            if (!ancestor->origin().is_same_origin(document.origin()))
                return false;
        }
        return true;
    };
    if (is_enabled())
        return true;

    // 4. Return false.
    return false;
}

u64 CanonicalWindowEnvironmentSettingsObject::agent_cluster_id() const
{
    return m_window.agent().agent_cluster_id();
}

CanonicalWorkerEnvironmentSettingsObject::CanonicalWorkerEnvironmentSettingsObject(Web::HTML::EnvironmentId id, Settings settings)
    : CanonicalEnvironmentSettingsObject(move(id), {}, move(settings.top_level_origin))
    , m_url(move(settings.url))
    , m_origin(move(settings.origin))
    , m_outside_settings_has_cross_site_ancestor(settings.outside_settings_has_cross_site_ancestor)
    , m_cross_origin_isolated_capability(settings.cross_origin_isolated_capability)
    , m_agent_cluster_id(settings.agent_cluster_id)
{
}

// https://html.spec.whatwg.org/multipage/workers.html#script-settings-for-workers:concept-settings-object-has-cross-site-ancestor
bool CanonicalWorkerEnvironmentSettingsObject::has_cross_site_ancestor() const
{
    // 1. If outside settings's has cross-site ancestor is true, then return true.
    if (m_outside_settings_has_cross_site_ancestor)
        return true;

    // 2. If worker global scope's url's scheme is "data", then return true.
    if (m_url.scheme() == "data"sv)
        return true;

    // 3. Return false.
    return false;
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key-for-non-storage-purposes
Web::StorageAPI::StorageKey obtain_a_storage_key_for_non_storage_purposes(CanonicalEnvironmentSettingsObject const& environment)
{
    // 1. Let origin be environment’s origin if environment is an environment settings object; otherwise environment’s
    //    creation URL’s origin.
    // 2. Return a tuple consisting of origin.
    return Web::StorageAPI::obtain_a_storage_key_for_non_storage_purposes(environment.origin());
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key
Optional<Web::StorageAPI::StorageKey> obtain_a_storage_key(CanonicalEnvironmentSettingsObject const& environment)
{
    // 1. Let key be the result of running obtain a storage key for non-storage purposes with environment.
    auto key = obtain_a_storage_key_for_non_storage_purposes(environment);

    // AD-HOC: file:// URLs are opaque, but other browsers support storage on file:// URLs.
    if (key.origin.is_opaque_file_origin())
        key.origin = URL::Origin { "file"_string, String {}, {} };

    // 2. If key’s origin is an opaque origin, then return failure.
    if (key.origin.is_opaque())
        return {};

    // FIXME: 3. If the user has disabled storage, then return failure.

    // 4. Return key.
    return key;
}

}
