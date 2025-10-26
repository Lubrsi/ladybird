/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <WebDriver/Client.h>
#include <WebDriver/Session.h>

#define WEBDRIVER_TRY(expression) \
    ({ \
        auto result = expression; \
        if (result.is_error()) [[unlikely]] { \
            on_complete(result.release_error()); \
            return; \
        } \
        result.release_value(); \
    })

namespace WebDriver {

ErrorOr<NonnullRefPtr<Client>> Client::try_create(NonnullOwnPtr<Core::BufferedTCPSocket> socket, LaunchBrowserCallback launch_browser_callback)
{
    if (!launch_browser_callback)
        return Error::from_string_literal("The callback to launch the browser must be provided");

    TRY(socket->set_blocking(true));
    return adopt_nonnull_ref_or_enomem(new (nothrow) Client(move(socket), move(launch_browser_callback)));
}

Client::Client(NonnullOwnPtr<Core::BufferedTCPSocket> socket, LaunchBrowserCallback launch_browser_callback)
    : Web::WebDriver::Client(move(socket))
    , m_launch_browser_callback(move(launch_browser_callback))
{
}

Client::~Client() = default;

// 8.1 New Session, https://w3c.github.io/webdriver/#dfn-new-sessions
// POST /session
void Client::new_session(Web::WebDriver::Parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session");

    // 1. If the implementation is an endpoint node, and the list of active HTTP sessions is not empty, or otherwise if
    //    the implementation is unable to start an additional session, return error with error code session not created.
    if (Session::session_count(Web::WebDriver::SessionFlags::Http) > 0) {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, "There is already an active HTTP session"sv));
        return;
    }

    // FIXME: 2. If the remote end is an intermediary node, take implementation-defined steps that either result in returning
    //           an error with error code session not created, or in returning a success with data that is isomorphic to that
    //           returned by remote ends according to the rest of this algorithm. If an error is not returned, the intermediary
    //           node must retain a reference to the session created on the upstream node as the associated session such that
    //           commands may be forwarded to this associated session on subsequent commands.

    // 3. Let flags be a set containing "http".
    static constexpr auto flags = Web::WebDriver::SessionFlags::Http;

    // 4. Let capabilities be the result of trying to process capabilities with parameters and flags.
    auto capabilities = WEBDRIVER_TRY(Web::WebDriver::process_capabilities(payload, flags));

    // 5. If capabilities's is null, return error with error code session not created.
    if (capabilities.is_null()) {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, "Could not match capabilities"sv));
        return;
    }

    // 6. Let session be the result of create a session, with capabilities, and flags.
    auto maybe_session = Session::create(*this, capabilities.as_object(), flags);
    if (maybe_session.is_error()) {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, MUST(String::formatted("Failed to start session: {}", maybe_session.error()))));
        return;
    }

    auto session = maybe_session.release_value();

    // 7. Let body be a JSON Object initialized with:
    JsonObject body;
    // "sessionId"
    //     session's session ID.
    body.set("sessionId"sv, JsonValue { session->session_id() });
    // "capabilities"
    //     capabilities
    body.set("capabilities"sv, move(capabilities));

    // 8. Set session' current top-level browsing context to one of the endpoint node's top-level browsing contexts,
    //    preferring the top-level browsing context that has system focus, or otherwise preferring any top-level
    //    browsing context whose visibility state is visible.
    // NOTE: This happens in the WebContent process.

    // FIXME: 9. Set the request queue to a new queue.

    // 10. Return success with data body.
    on_complete(JsonValue { move(body) });
}

// 8.2 Delete Session, https://w3c.github.io/webdriver/#dfn-delete-session
// DELETE /session/{session id}
void Client::delete_session(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>");

    // 1. If session is an active HTTP session, try to close the session with session.
    if (auto session = Session::find_session(parameters[0], Web::WebDriver::SessionFlags::Http, Session::AllowInvalidWindowHandle::Yes); !session.is_error())
        session.value()->close();

    // 2. Return success with data null.
    on_complete(JsonValue {});
}

// https://w3c.github.io/webdriver/#dfn-readiness-state
static bool readiness_state()
{
    // The readiness state of a remote end indicates whether it is free to accept new connections. It must be false if
    // the implementation is an endpoint node and the list of active HTTP sessions is not empty, or otherwise if the
    // remote end is known to be in a state in which attempting to create new sessions would fail. In all other cases it
    // must be true.
    return Session::session_count(Web::WebDriver::SessionFlags::Http) == 0;
}

// 8.3 Status, https://w3c.github.io/webdriver/#dfn-status
// GET /status
void Client::get_status(Web::WebDriver::Parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /status");

    auto readiness_state = WebDriver::readiness_state();

    // 1. Let body be a new JSON Object with the following properties:
    //    "ready"
    //        The remote end's readiness state.
    //    "message"
    //        An implementation-defined string explaining the remote end's readiness state.
    JsonObject body;
    body.set("ready"sv, readiness_state);
    body.set("message"sv, MUST(String::formatted("{} to accept a new session", readiness_state ? "Ready"sv : "Not ready"sv)));

    // 2. Return success with data body.
    on_complete(JsonValue { body });
}

// 9.1 Get Timeouts, https://w3c.github.io/webdriver/#dfn-get-timeouts
// GET /session/{session id}/timeouts
void Client::get_timeouts(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session id>/timeouts");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));
    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_timeouts(request_id);
    }, move(on_complete));
}

// 9.2 Set Timeouts, https://w3c.github.io/webdriver/#dfn-set-timeouts
// POST /session/{session id}/timeouts
void Client::set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session id>/timeouts");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));
    session->set_timeouts(move(payload), move(on_complete));
}

// 10.1 Navigate To, https://w3c.github.io/webdriver/#dfn-navigate-to
// POST /session/{session id}/url
void Client::navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/url");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_navigate_to(request_id, move(payload));
    }, move(on_complete));
}

// 10.2 Get Current URL, https://w3c.github.io/webdriver/#dfn-get-current-url
// GET /session/{session id}/url
void Client::get_current_url(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/url");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_current_url(request_id);
    }, move(on_complete));
}

// 10.3 Back, https://w3c.github.io/webdriver/#dfn-back
// POST /session/{session id}/back
void Client::back(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/back");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_back(request_id);
    }, move(on_complete));
}

// 10.4 Forward, https://w3c.github.io/webdriver/#dfn-forward
// POST /session/{session id}/forward
void Client::forward(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/forward");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_forward(request_id);
    }, move(on_complete));
}

// 10.5 Refresh, https://w3c.github.io/webdriver/#dfn-refresh
// POST /session/{session id}/refresh
void Client::refresh(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/refresh");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_refresh(request_id);
    }, move(on_complete));
}

// 10.6 Get Title, https://w3c.github.io/webdriver/#dfn-get-title
// GET /session/{session id}/title
void Client::get_title(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/title");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_title(request_id);
    }, move(on_complete));
}

// 11.1 Get Window Handle, https://w3c.github.io/webdriver/#get-window-handle
// GET /session/{session id}/window
void Client::get_window_handle(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/window");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_ensure_top_level_browsing_context_is_open(request_id);
    }, [on_complete = move(on_complete), session](auto) {
        // 2. Return success with data being the window handle associated with the current top-level browsing context.
        on_complete(JsonValue { session->current_window_handle() });
    });
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
// DELETE /session/{session id}/window
void Client::close_window(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/window");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));
    session->close_window(move(on_complete));
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
// POST /session/{session id}/window
void Client::switch_to_window(Web::WebDriver::Parameters parameters, AK::JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0], Web::WebDriver::SessionFlags::Default, Session::AllowInvalidWindowHandle::Yes));

    if (!payload.is_object()) [[unlikely]] {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload is not a JSON object"sv));
        return;
    }

    // 1. Let handle be the result of getting the property "handle" from the parameters argument.
    auto handle = payload.as_object().get("handle"sv);

    // 2. If handle is undefined, return error with error code invalid argument.
    if (!handle.has_value()) [[unlikely]] {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "No property called 'handle' present"sv));
        return;
    }

    session->switch_to_window(handle->as_string(), move(on_complete));
}

// 11.4 Get Window Handles, https://w3c.github.io/webdriver/#dfn-get-window-handles
// GET /session/{session id}/window/handles
void Client::get_window_handles(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/window/handles");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0], Web::WebDriver::SessionFlags::Default, Session::AllowInvalidWindowHandle::Yes));
    on_complete(session->get_window_handles());
}

// 11.5 New Window, https://w3c.github.io/webdriver/#dfn-new-window
// POST /session/{session id}/window/new
void Client::new_window(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/new");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_new_window(request_id, move(payload));
    }, [on_complete = move(on_complete)](Web::WebDriver::Response handle_response) {
        auto handle = WEBDRIVER_TRY(move(handle_response));

        static constexpr u32 CONNECTION_TIMEOUT_MS = 5000;
        auto timeout_fired = false;
        auto timer = Core::Timer::create_single_shot(CONNECTION_TIMEOUT_MS, [&timeout_fired] { timeout_fired = true; });
        timer->start();

        Core::EventLoop::current().spin_until([&session, &timeout_fired, handle = handle.as_object().get("handle"sv)->as_string()]() {
            return session->has_window_handle(handle) || timeout_fired;
        });

        if (timeout_fired)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::Timeout, "Timed out waiting for window handle"sv);

        return handle;
    });
}

// 11.6 Switch To Frame, https://w3c.github.io/webdriver/#dfn-switch-to-frame
// POST /session/{session id}/frame
void Client::switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/frame");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_switch_to_frame(request_id, move(payload));
    }, move(on_complete));
}

// 11.7 Switch To Parent Frame, https://w3c.github.io/webdriver/#dfn-switch-to-parent-frame
// POST /session/{session id}/frame/parent
void Client::switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/frame/parent");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_switch_to_parent_frame(request_id, move(payload));
    }, move(on_complete));
}

// 11.8.1 Get Window Rect, https://w3c.github.io/webdriver/#dfn-get-window-rect
// GET /session/{session id}/window/rect
void Client::get_window_rect(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/window/rect");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_window_rect(request_id);
    }, move(on_complete));
}

// 11.8.2 Set Window Rect, https://w3c.github.io/webdriver/#dfn-set-window-rect
// POST /session/{session id}/window/rect
void Client::set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/rect");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_set_window_rect(request_id, move(payload));
    }, move(on_complete));
}

// 11.8.3 Maximize Window, https://w3c.github.io/webdriver/#dfn-maximize-window
// POST /session/{session id}/window/maximize
void Client::maximize_window(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/maximize");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([&](auto& connection, auto request_id) {
        connection.async_maximize_window(request_id);
    }, move(on_complete));
}

// 11.8.4 Minimize Window, https://w3c.github.io/webdriver/#minimize-window
// POST /session/{session id}/window/minimize
void Client::minimize_window(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/minimize");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_minimize_window(request_id);
    }, move(on_complete));
}

// 11.8.5 Fullscreen Window, https://w3c.github.io/webdriver/#dfn-fullscreen-window
// POST /session/{session id}/window/fullscreen
void Client::fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/fullscreen");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        return connection.async_fullscreen_window(request_id);
    }, move(on_complete));
}

// Extension: Consume User Activation, https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
// POST /session/{session id}/window/consume-user-activation
void Client::consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/window/consume-user-activation");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_consume_user_activation(request_id);
    }, move(on_complete));
}

// 12.3.2 Find Element, https://w3c.github.io/webdriver/#dfn-find-element
// POST /session/{session id}/element
void Client::find_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_find_element(request_id, move(payload));
    }, move(on_complete));
}

// 12.3.3 Find Elements, https://w3c.github.io/webdriver/#dfn-find-elements
// POST /session/{session id}/elements
void Client::find_elements(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/elements");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_find_elements(request_id, move(payload));
    }, move(on_complete));
}

// 12.3.4 Find Element From Element, https://w3c.github.io/webdriver/#dfn-find-element-from-element
// POST /session/{session id}/element/{element id}/element
void Client::find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/element");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters), payload = move(payload)](auto& connection, auto request_id) {
        connection.async_find_element_from_element(request_id, move(payload), move(parameters[1]));
    }, move(on_complete));
}

// 12.3.5 Find Elements From Element, https://w3c.github.io/webdriver/#dfn-find-elements-from-element
// POST /session/{session id}/element/{element id}/elements
void Client::find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/elements");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters), payload = move(payload)](auto& connection, auto request_id) {
        connection.async_find_elements_from_element(request_id, move(payload), move(parameters[1]));
    }, move(on_complete));
}

// 12.3.6 Find Element From Shadow Root, https://w3c.github.io/webdriver/#find-element-from-shadow-root
// POST /session/{session id}/shadow/{shadow id}/element
void Client::find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/shadow/<shadow_id>/element");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters), payload = move(payload)](auto& connection, auto request_id) {
        connection.async_find_element_from_shadow_root(request_id, move(payload), move(parameters[1]));
    }, move(on_complete));
}

// 12.3.7 Find Elements From Shadow Root, https://w3c.github.io/webdriver/#find-elements-from-shadow-root
// POST /session/{session id}/shadow/{shadow id}/elements
void Client::find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/shadow/<shadow_id>/elements");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters), payload = move(payload)](auto& connection, auto request_id) {
        connection.async_find_elements_from_shadow_root(request_id, move(payload), move(parameters[1]));
    }, move(on_complete));
}

// 12.3.8 Get Active Element, https://w3c.github.io/webdriver/#get-active-element
// GET /session/{session id}/element/active
void Client::get_active_element(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/active");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_active_element(request_id);
    }, move(on_complete));
}

// 12.3.9 Get Element Shadow Root, https://w3c.github.io/webdriver/#get-element-shadow-root
// GET /session/{session id}/element/{element id}/shadow
void Client::get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/shadow");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_shadow_root(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.1 Is Element Selected, https://w3c.github.io/webdriver/#dfn-is-element-selected
// GET /session/{session id}/element/{element id}/selected
void Client::is_element_selected(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/selected");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_is_element_selected(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.2 Get Element Attribute, https://w3c.github.io/webdriver/#dfn-get-element-attribute
// GET /session/{session id}/element/{element id}/attribute/{name}
void Client::get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/attribute/<name>");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_attribute(request_id, move(parameters[1]), move(parameters[2]));
    }, move(on_complete));
}

// 12.4.3 Get Element Property, https://w3c.github.io/webdriver/#dfn-get-element-property
// GET /session/{session id}/element/{element id}/property/{name}
void Client::get_element_property(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/property/<name>");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_property(request_id, move(parameters[1]), move(parameters[2]));
    }, move(on_complete));
}

// 12.4.4 Get Element CSS Value, https://w3c.github.io/webdriver/#dfn-get-element-css-value
// GET /session/{session id}/element/{element id}/css/{property name}
void Client::get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/css/<property_name>");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_css_value(request_id, move(parameters[1]), move(parameters[2]));
    }, move(on_complete));
}

// 12.4.5 Get Element Text, https://w3c.github.io/webdriver/#dfn-get-element-text
// GET /session/{session id}/element/{element id}/text
void Client::get_element_text(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/text");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_text(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.6 Get Element Tag Name, https://w3c.github.io/webdriver/#dfn-get-element-tag-name
// GET /session/{session id}/element/{element id}/name
void Client::get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/name");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_tag_name(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.7 Get Element Rect, https://w3c.github.io/webdriver/#dfn-get-element-rect
// GET /session/{session id}/element/{element id}/rect
void Client::get_element_rect(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/rect");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_element_rect(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.8 Is Element Enabled, https://w3c.github.io/webdriver/#dfn-is-element-enabled
// GET /session/{session id}/element/{element id}/enabled
void Client::is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/enabled");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_is_element_enabled(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.9 https://w3c.github.io/webdriver/#dfn-get-computed-role
// GET /session/{session id}/element/{element id}/computedrole
void Client::get_computed_role(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session id>/element/<element id>/computedrole");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_computed_role(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.4.10 Get Computed Label, https://w3c.github.io/webdriver/#get-computed-label
// GET /session/{session id}/element/{element id}/computedlabel
void Client::get_computed_label(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session id>/element/<element id>/computedlabel");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_computed_label(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.5.1 Element Click, https://w3c.github.io/webdriver/#element-click
// POST /session/{session id}/element/{element id}/click
void Client::element_click(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/click");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_element_click(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.5.2 Element Clear, https://w3c.github.io/webdriver/#dfn-element-clear
// POST /session/{session id}/element/{element id}/clear
void Client::element_clear(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/clear");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_element_clear(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 12.5.3 Element Send Keys, https://w3c.github.io/webdriver/#dfn-element-send-keys
// POST /session/{session id}/element/{element id}/value
void Client::element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/element/<element_id>/value");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters), payload = move(payload)](auto& connection, auto request_id) {
        connection.async_element_send_keys(request_id, move(parameters[1]), move(payload));
    }, move(on_complete));
}

// 13.1 Get Page Source, https://w3c.github.io/webdriver/#dfn-get-page-source
// GET /session/{session id}/source
void Client::get_source(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/source");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_source(request_id);
    }, move(on_complete));
}

// 13.2.1 Execute Script, https://w3c.github.io/webdriver/#dfn-execute-script
// POST /session/{session id}/execute/sync
void Client::execute_script(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/execute/sync");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_execute_script(request_id, move(payload));
    }, move(on_complete));
}

// 13.2.2 Execute Async Script, https://w3c.github.io/webdriver/#dfn-execute-async-script
// POST /session/{session id}/execute/async
void Client::execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/execute/async");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_execute_async_script(request_id, move(payload));
    }, move(on_complete));
}

// 14.1 Get All Cookies, https://w3c.github.io/webdriver/#dfn-get-all-cookies
// GET /session/{session id}/cookie
void Client::get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/cookie");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_all_cookies(request_id);
    }, move(on_complete));
}

// 14.2 Get Named Cookie, https://w3c.github.io/webdriver/#dfn-get-named-cookie
// GET /session/{session id}/cookie/{name}
void Client::get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/cookie/<name>");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_get_named_cookie(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 14.3 Add Cookie, https://w3c.github.io/webdriver/#dfn-adding-a-cookie
// POST /session/{session id}/cookie
void Client::add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/cookie");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_add_cookie(request_id, move(payload));
    }, move(on_complete));
}

// 14.4 Delete Cookie, https://w3c.github.io/webdriver/#dfn-delete-cookie
// DELETE /session/{session id}/cookie/{name}
void Client::delete_cookie(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/cookie/<name>");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_delete_cookie(request_id, parameters[1]);
    }, move(on_complete));
}

// 14.5 Delete All Cookies, https://w3c.github.io/webdriver/#dfn-delete-all-cookies
// DELETE /session/{session id}/cookie
void Client::delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/cookie");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_delete_all_cookies(request_id);
    }, move(on_complete));
}

// 15.7 Perform Actions, https://w3c.github.io/webdriver/#perform-actions
// POST /session/{session id}/actions
void Client::perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/actions");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_perform_actions(request_id, move(payload));
    }, move(on_complete));
}

// 15.8 Release Actions, https://w3c.github.io/webdriver/#release-actions
// DELETE /session/{session id}/actions
void Client::release_actions(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling DELETE /session/<session_id>/actions");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_release_actions(request_id);
    }, move(on_complete));
}

// 16.1 Dismiss Alert, https://w3c.github.io/webdriver/#dismiss-alert
// POST /session/{session id}/alert/dismiss
void Client::dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/alert/dismiss");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_dismiss_alert(request_id);
    }, move(on_complete));
}

// 16.2 Accept Alert, https://w3c.github.io/webdriver/#accept-alert
// POST /session/{session id}/alert/accept
void Client::accept_alert(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/alert/accept");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_accept_alert(request_id);
    }, move(on_complete));
}

// 16.3 Get Alert Text, https://w3c.github.io/webdriver/#get-alert-text
// GET /session/{session id}/alert/text
void Client::get_alert_text(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/alert/text");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));
    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_get_alert_text(request_id);
    }, move(on_complete));
}

// 16.4 Send Alert Text, https://w3c.github.io/webdriver/#send-alert-text
// POST /session/{session id}/alert/text
void Client::send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session_id>/alert/text");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));
    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_send_alert_text(request_id, move(payload));
    }, move(on_complete));
}

// 17.1 Take Screenshot, https://w3c.github.io/webdriver/#take-screenshot
// GET /session/{session id}/screenshot
void Client::take_screenshot(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/screenshot");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([](auto& connection, auto request_id) {
        connection.async_take_screenshot(request_id);
    }, move(on_complete));
}

// 17.2 Take Element Screenshot, https://w3c.github.io/webdriver/#dfn-take-element-screenshot
// GET /session/{session id}/element/{element id}/screenshot
void Client::take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling GET /session/<session_id>/element/<element_id>/screenshot");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));

    session->perform_async_action([parameters = move(parameters)](auto& connection, auto request_id) {
        connection.async_take_element_screenshot(request_id, move(parameters[1]));
    }, move(on_complete));
}

// 18.1 Print Page, https://w3c.github.io/webdriver/#dfn-print-page
// POST /session/{session id}/print
void Client::print_page(Web::WebDriver::Parameters parameters, JsonValue payload, Function<void(Web::WebDriver::Response)> on_complete)
{
    dbgln_if(WEBDRIVER_DEBUG, "Handling POST /session/<session id>/print");
    auto session = WEBDRIVER_TRY(Session::find_session(parameters[0]));
    session->perform_async_action([payload = move(payload)](auto& connection, auto request_id) {
        connection.async_print_page(request_id, move(payload));
    }, move(on_complete));
}

}
