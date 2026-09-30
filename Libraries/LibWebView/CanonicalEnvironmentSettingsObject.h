/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
class WEBVIEW_API CanonicalEnvironmentSettingsObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CanonicalEnvironmentSettingsObject() = default;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    Web::HTML::EnvironmentId const& id() const { return m_id; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-creation-url
    Optional<URL::URL> const& top_level_creation_url() const { return m_top_level_creation_url; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-top-level-origin
    Optional<URL::Origin> const& top_level_origin() const { return m_top_level_origin; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-origin
    virtual URL::Origin const& origin() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-has-cross-site-ancestor
    virtual bool has_cross_site_ancestor() const = 0;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-cross-origin-isolated-capability
    virtual Web::HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const = 0;

    virtual u64 agent_cluster_id() const = 0;

    bool may_use_cookies_of(URL::URL const&) const;

    // Replaces the settings held here in a serialization of this environment.
    void serialize_into(Web::HTML::SerializedEnvironmentSettingsObject&) const;

protected:
    CanonicalEnvironmentSettingsObject(Web::HTML::EnvironmentId id, Optional<URL::URL> top_level_creation_url, Optional<URL::Origin> top_level_origin)
        : m_id(move(id))
        , m_top_level_creation_url(move(top_level_creation_url))
        , m_top_level_origin(move(top_level_origin))
    {
    }

private:
    Web::HTML::EnvironmentId m_id;
    Optional<URL::URL> m_top_level_creation_url;
    Optional<URL::Origin> m_top_level_origin;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects
class WEBVIEW_API CanonicalWindowEnvironmentSettingsObject final : public CanonicalEnvironmentSettingsObject {
public:
    CanonicalWindowEnvironmentSettingsObject(CanonicalWindow&, Web::HTML::EnvironmentId id, URL::URL top_level_creation_url, URL::Origin top_level_origin, RefPtr<CanonicalDocument const> container_document);

    virtual URL::Origin const& origin() const override;
    virtual bool has_cross_site_ancestor() const override;
    virtual Web::HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override;
    virtual u64 agent_cluster_id() const override;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container-document
    // The container document of the navigable the window was created in, if it has a container.
    CanonicalDocument const* container_document() const { return m_container_document.ptr(); }

private:
    bool associated_document_is_allowed_to_use_the_cross_origin_isolated_feature() const;

    CanonicalWindow& m_window;
    RefPtr<CanonicalDocument const> m_container_document;
};

// https://html.spec.whatwg.org/multipage/workers.html#script-settings-for-workers
class WEBVIEW_API CanonicalWorkerEnvironmentSettingsObject final : public CanonicalEnvironmentSettingsObject {
public:
    struct Settings {
        URL::URL url;
        URL::Origin origin;
        Optional<URL::Origin> top_level_origin;
        bool outside_settings_has_cross_site_ancestor { false };
        Web::HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability { Web::HTML::CanUseCrossOriginIsolatedAPIs::No };
        u64 agent_cluster_id { 0 };
    };

    CanonicalWorkerEnvironmentSettingsObject(Web::HTML::EnvironmentId id, Settings);

    virtual URL::Origin const& origin() const override { return m_origin; }
    virtual bool has_cross_site_ancestor() const override;
    virtual Web::HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override { return m_cross_origin_isolated_capability; }
    virtual u64 agent_cluster_id() const override { return m_agent_cluster_id; }

private:
    URL::URL m_url;
    URL::Origin m_origin;
    bool m_outside_settings_has_cross_site_ancestor { false };
    Web::HTML::CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { Web::HTML::CanUseCrossOriginIsolatedAPIs::No };
    u64 m_agent_cluster_id { 0 };
};

WEBVIEW_API Web::StorageAPI::StorageKey obtain_a_storage_key_for_non_storage_purposes(CanonicalEnvironmentSettingsObject const&);
WEBVIEW_API Optional<Web::StorageAPI::StorageKey> obtain_a_storage_key(CanonicalEnvironmentSettingsObject const&);

}
