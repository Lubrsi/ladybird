/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Process.h>
#include <LibWeb/WebDriver/Client.h>
#include <LibWeb/WebDriver/Response.h>

namespace WebDriver {

using LaunchBrowserCallback = Function<ErrorOr<Core::Process>(ByteString const& socket_path, bool headless)>;

class Client final : public Web::WebDriver::Client {
    C_OBJECT_ABSTRACT(Client);

public:
    static ErrorOr<NonnullRefPtr<Client>> try_create(NonnullOwnPtr<Core::BufferedTCPSocket>, LaunchBrowserCallback);
    virtual ~Client() override;

    LaunchBrowserCallback const& launch_browser_callback() const { return m_launch_browser_callback; }

private:
    Client(NonnullOwnPtr<Core::BufferedTCPSocket>, LaunchBrowserCallback);

    virtual void new_session(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void delete_session(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_status(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_current_url(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void back(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void forward(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void refresh(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_title(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_window_handle(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void close_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void switch_to_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_window_handles(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void new_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void maximize_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void minimize_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void find_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void find_elements(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_active_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void is_element_selected(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_property(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_text(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_element_rect(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_computed_role(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_computed_label(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void element_click(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void element_clear(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_source(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void execute_script(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void delete_cookie(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void release_actions(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void accept_alert(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void get_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void take_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;
    virtual void print_page(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete) override;

    LaunchBrowserCallback m_launch_browser_callback;
};

}
