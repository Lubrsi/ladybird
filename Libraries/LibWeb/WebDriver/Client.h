/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Socket.h>
#include <LibHTTP/Forward.h>
#include <LibHTTP/HttpRequest.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

using Parameters = Vector<String>;

class WEB_API Client : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(Client);

public:
    virtual ~Client();

    // 8. Sessions, https://w3c.github.io/webdriver/#sessions
    virtual void new_session(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void delete_session(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_status(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 9. Timeouts, https://w3c.github.io/webdriver/#timeouts
    virtual void get_timeouts(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void set_timeouts(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 10. Navigation, https://w3c.github.io/webdriver/#navigation
    virtual void navigate_to(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_current_url(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void back(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void forward(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void refresh(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_title(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 11. Contexts, https://w3c.github.io/webdriver/#contexts
    virtual void get_window_handle(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void close_window(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void new_window(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void switch_to_window(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_window_handles(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_window_rect(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void set_window_rect(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void maximize_window(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void minimize_window(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void fullscreen_window(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void switch_to_frame(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void switch_to_parent_frame(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // Extension: https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
    virtual void consume_user_activation(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 12. Elements, https://w3c.github.io/webdriver/#elements
    virtual void find_element(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void find_elements(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void find_element_from_element(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void find_elements_from_element(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void find_element_from_shadow_root(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void find_elements_from_shadow_root(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_active_element(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_shadow_root(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void is_element_selected(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_attribute(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_property(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_css_value(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_text(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_tag_name(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_element_rect(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void is_element_enabled(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_computed_role(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_computed_label(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void element_click(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void element_clear(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void element_send_keys(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 13. Document, https://w3c.github.io/webdriver/#document
    virtual void get_source(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void execute_script(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void execute_async_script(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 14. Cookies, https://w3c.github.io/webdriver/#cookies
    virtual void get_all_cookies(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_named_cookie(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void add_cookie(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void delete_cookie(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void delete_all_cookies(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 15. Actions, https://w3c.github.io/webdriver/#actions
    virtual void perform_actions(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void release_actions(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 16. User prompts, https://w3c.github.io/webdriver/#user-prompts
    virtual void dismiss_alert(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void accept_alert(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void get_alert_text(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void send_alert_text(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 17. Screen capture, https://w3c.github.io/webdriver/#screen-capture
    virtual void take_screenshot(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;
    virtual void take_element_screenshot(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    // 18. Print, https://w3c.github.io/webdriver/#print
    virtual void print_page(Parameters parameters, JsonValue payload, Function<void(Response)> on_complete) = 0;

    Function<void()> on_death;

protected:
    explicit Client(NonnullOwnPtr<Core::BufferedTCPSocket>);

private:
    using WrappedError = Variant<AK::Error, HTTP::HttpRequest::ParseError, WebDriver::Error>;

    void die();

    ErrorOr<void, WrappedError> on_ready_to_read();
    static ErrorOr<JsonValue, WrappedError> read_body_as_json(HTTP::HttpRequest const&);

    ErrorOr<void, WrappedError> handle_request(HTTP::HttpRequest const&, JsonValue body);
    void handle_error(HTTP::HttpRequest const&, WrappedError const&);

    ErrorOr<void, WrappedError> send_success_response(HTTP::HttpRequest const&, JsonValue result);
    ErrorOr<void, WrappedError> send_error_response(HTTP::HttpRequest const&, Error const& error);
    static void log_response(HTTP::HttpRequest const&, unsigned code);

    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
    StringBuilder m_remaining_request;
};

}
