/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibURL/Parser.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Menu.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/ViewImplementation.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#    include <LibCore/MachPort.h>
#endif

namespace WebView {

static HashMap<u64, ViewImplementation*> s_all_views;
static u64 s_view_count = 1; // This has to start at 1 for Firefox DevTools.

void ViewImplementation::for_each_view(Function<IterationDecision(ViewImplementation&)> callback)
{
    for (auto& view : s_all_views) {
        if (callback(*view.value) == IterationDecision::Break)
            break;
    }
}

Optional<ViewImplementation&> ViewImplementation::find_view_by_id(u64 id)
{
    if (auto view = s_all_views.get(id); view.has_value())
        return *view.value();
    return {};
}

ViewImplementation::ViewImplementation()
    : m_document_cookie_version_buffer(Core::create_shared_version_buffer())
    , m_view_id(s_view_count++)
{
    s_all_views.set(m_view_id, this);

    initialize_context_menus();

    m_repeated_crash_timer = Core::Timer::create_single_shot(1000, [this] {
        // Reset the "crashing a lot" counter after 1 second in case we just
        // happen to be visiting crashy websites a lot.
        this->m_crash_count = 0;
    });

    on_request_file = [this](auto const& path, auto request_id) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);

        if (file.is_error())
            client().async_handle_file_return(page_id(), file.error().code(), {}, request_id);
        else
            client().async_handle_file_return(page_id(), 0, IPC::File::adopt_file(file.release_value()), request_id);
    };
}

ViewImplementation::~ViewImplementation()
{
    s_all_views.remove(m_view_id);

    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);
}

WebContentClient& ViewImplementation::client()
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

WebContentClient const& ViewImplementation::client() const
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

u64 ViewImplementation::page_id() const
{
    VERIFY(m_client_state.client);
    return m_client_state.page_index;
}

void ViewImplementation::create_new_process_for_cross_site_navigation(URL::URL const& url)
{
    if (m_client_state.client) {
        m_client_state.client->unregister_view(m_client_state.page_index);
        client().async_close_server();
    }

    initialize_client();
    VERIFY(m_client_state.client);

    if (on_web_content_process_change_for_cross_site_navigation)
        on_web_content_process_change_for_cross_site_navigation();

    // The old WebContent process is gone — clear any in-flight traversal/operation state.
    m_active_traversal = {};
    m_queue_jump_traversal = {};
    m_active_operation = false;
    m_session_history_traversal_queue.clear();
    m_traversal_exclusion_set.clear();
    m_running_nested_queue_jump = false;

    // Don't keep a stale backup bitmap around.
    m_backup_bitmap = nullptr;
    handle_resize();

    // Restore session history from the UI-side mirror into the new WebContent process.
    // This preserves back/forward history across cross-site process switches.
    // IPC ordering guarantees restore is processed before the subsequent load_url.
    if (!m_session_history_entries.is_empty())
        client().async_restore_session_history(page_id(), m_session_history_current_step, m_session_history_entries);

    load(url);
}

void ViewImplementation::server_did_paint(Badge<WebContentClient>, i32 bitmap_id, Gfx::IntSize size)
{
    if (m_client_state.back_bitmap.id == bitmap_id) {
        m_client_state.has_usable_bitmap = true;
        m_client_state.back_bitmap.last_painted_size = size.to_type<Web::DevicePixels>();
        swap(m_client_state.back_bitmap, m_client_state.front_bitmap);
        m_backup_bitmap = nullptr;
        if (on_ready_to_paint)
            on_ready_to_paint();
    }

    client().async_ready_to_paint(page_id());
}

void ViewImplementation::set_window_position(Gfx::IntPoint position)
{
    client().async_set_window_position(m_client_state.page_index, position.to_type<Web::DevicePixels>());
}

void ViewImplementation::set_window_size(Gfx::IntSize size)
{
    client().async_set_window_size(m_client_state.page_index, size.to_type<Web::DevicePixels>());
}

void ViewImplementation::did_update_window_rect()
{
    client().async_did_update_window_rect(m_client_state.page_index);
}

void ViewImplementation::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    m_system_visibility_state = visibility_state;
    client().async_set_system_visibility_state(m_client_state.page_index, m_system_visibility_state);
}

void ViewImplementation::load(URL::URL const& url)
{
    m_url = url;
    client().async_load_url(page_id(), url);
}

void ViewImplementation::load_html(StringView html)
{
    client().async_load_html(page_id(), html);
}

void ViewImplementation::reload()
{
    client().async_reload(page_id());
}

void ViewImplementation::traverse_the_history_by_delta(int delta)
{
    auto all_steps = get_all_used_history_steps(m_session_history_entries);
    auto current_index = all_steps.find_first_index(m_session_history_current_step);
    if (!current_index.has_value())
        return;
    auto target_index = static_cast<int>(*current_index) + delta;
    if (target_index < 0 || target_index >= static_cast<int>(all_steps.size()))
        return;

    TraversalCommand cmd;
    cmd.target_step = all_steps[target_index];
    cmd.check_for_cancelation = true;
    cmd.navigation_type = Web::Bindings::NavigationType::Traverse;
    cmd.user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI;
    m_session_history_traversal_queue.append(SessionHistoryCommand { cmd });
    process_next_session_history_command();
}

void ViewImplementation::zoom_in()
{
    if (m_zoom_level >= ZOOM_MAX_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level + ZOOM_STEP) * 100) / 100.0;
    update_zoom();
}

void ViewImplementation::zoom_out()
{
    if (m_zoom_level <= ZOOM_MIN_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level - ZOOM_STEP) * 100) / 100.0;
    update_zoom();
}

void ViewImplementation::set_zoom(double zoom_level)
{
    m_zoom_level = max(ZOOM_MIN_LEVEL, min(zoom_level, ZOOM_MAX_LEVEL));
    update_zoom();
}

void ViewImplementation::reset_zoom()
{
    m_zoom_level = 1.0;
    update_zoom();
    client().async_reset_zoom(m_client_state.page_index);
}

void ViewImplementation::enqueue_input_event(Web::InputEvent event)
{
    // Send the next event over to the WebContent to be handled by JS. We'll later get a message to say whether JS
    // prevented the default event behavior, at which point we either discard or handle that event, and then try to
    // process the next one.
    m_pending_input_events.enqueue(move(event));

    m_pending_input_events.tail().visit(
        [this](Web::KeyEvent const& event) {
            client().async_key_event(m_client_state.page_index, event.clone_without_browser_data());
        },
        [this](Web::MouseEvent const& event) {
            client().async_mouse_event(m_client_state.page_index, event.clone_without_browser_data());
        },
        [this](Web::DragEvent& event) {
            auto cloned_event = event.clone_without_browser_data();
            cloned_event.files = move(event.files);

            client().async_drag_event(m_client_state.page_index, cloned_event);
        },
        [this](Web::PinchEvent const& event) {
            client().async_pinch_event(m_client_state.page_index, event);
        });
}

void ViewImplementation::did_finish_handling_input_event(Badge<WebContentClient>, Web::EventResult event_result)
{
    auto event = m_pending_input_events.dequeue();

    if (event_result == Web::EventResult::Handled)
        return;

    // Here we handle events that were not consumed or cancelled by the WebContent. Propagate the event back
    // to the concrete view implementation.
    event.visit(
        [this](Web::KeyEvent const& event) {
            if (on_finish_handling_key_event)
                on_finish_handling_key_event(event);
        },
        [this](Web::DragEvent const& event) {
            if (on_finish_handling_drag_event)
                on_finish_handling_drag_event(event);
        },
        [](auto const&) {});
}

void ViewImplementation::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    client().async_set_preferred_color_scheme(page_id(), color_scheme);
}

void ViewImplementation::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    client().async_set_preferred_contrast(page_id(), contrast);
}

void ViewImplementation::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    client().async_set_preferred_motion(page_id(), motion);
}

void ViewImplementation::notify_cookies_changed(HashTable<String> const& changed_domains, ReadonlySpan<HTTP::Cookie::Cookie> cookies)
{
    for (auto const& domain : changed_domains) {
        if (auto document_index = m_document_cookie_version_indices.get(domain); document_index.has_value())
            Core::increment_shared_version(m_document_cookie_version_buffer, *document_index);
    }

    if (!cookies.is_empty())
        client().async_cookies_changed(page_id(), cookies);
}

ErrorOr<Core::SharedVersionIndex> ViewImplementation::ensure_document_cookie_version_index(Badge<WebContentClient>, String const& domain)
{
    return m_document_cookie_version_indices.try_ensure(domain, [&]() -> ErrorOr<Core::SharedVersionIndex> {
        Core::SharedVersionIndex document_index = m_document_cookie_version_indices.size();

        if (!Core::initialize_shared_version(m_document_cookie_version_buffer, document_index)) {
            dbgln("Reached maximum document cookie version count for {}, cannot create new version for {}", m_url, domain);
            return Error::from_string_literal("Reached maximum document cookie version count");
        }

        return document_index;
    });
}

Optional<Core::SharedVersion> ViewImplementation::document_cookie_version(URL::URL const& url) const
{
    auto domain = HTTP::Cookie::canonicalize_domain(url);
    if (!domain.has_value())
        return {};

    auto document_index = m_document_cookie_version_indices.get(*domain);
    if (!document_index.has_value())
        return {};

    return Core::get_shared_version(m_document_cookie_version_buffer, *document_index);
}

ByteString ViewImplementation::selected_text()
{
    return client().get_selected_text(page_id());
}

Optional<String> ViewImplementation::selected_text_with_whitespace_collapsed()
{
    auto selected_text = MUST(Web::Infra::strip_and_collapse_whitespace(this->selected_text()));
    if (selected_text.is_empty())
        return OptionalNone {};
    return selected_text;
}

void ViewImplementation::select_all()
{
    client().async_select_all(page_id());
}

void ViewImplementation::find_in_page(String const& query, CaseSensitivity case_sensitivity)
{
    client().async_find_in_page(page_id(), query, case_sensitivity);
}

void ViewImplementation::find_in_page_next_match()
{
    client().async_find_in_page_next_match(page_id());
}

void ViewImplementation::find_in_page_previous_match()
{
    client().async_find_in_page_previous_match(page_id());
}

void ViewImplementation::get_source()
{
    client().async_get_source(page_id());
}

void ViewImplementation::inspect_dom_tree()
{
    client().async_inspect_dom_tree(page_id());
}

void ViewImplementation::inspect_accessibility_tree()
{
    client().async_inspect_accessibility_tree(page_id());
}

void ViewImplementation::get_hovered_node_id()
{
    client().async_get_hovered_node_id(page_id());
}

void ViewImplementation::inspect_dom_node(Web::UniqueNodeID node_id, DOMNodeProperties::Type property_type, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    client().async_inspect_dom_node(page_id(), property_type, node_id, pseudo_element);
}

void ViewImplementation::clear_inspected_dom_node()
{
    client().async_clear_inspected_dom_node(page_id());
}

void ViewImplementation::highlight_dom_node(Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    client().async_highlight_dom_node(page_id(), node_id, pseudo_element);
}

void ViewImplementation::clear_highlighted_dom_node()
{
    highlight_dom_node(0, {});
}

void ViewImplementation::set_listen_for_dom_mutations(bool listen_for_dom_mutations)
{
    client().async_set_listen_for_dom_mutations(page_id(), listen_for_dom_mutations);
}

void ViewImplementation::did_connect_devtools_client()
{
    m_devtools_connected = true;
    client().async_did_connect_devtools_client(page_id());
}

void ViewImplementation::did_disconnect_devtools_client()
{
    m_devtools_connected = false;
    client().async_did_disconnect_devtools_client(page_id());
}

void ViewImplementation::get_dom_node_inner_html(Web::UniqueNodeID node_id)
{
    client().async_get_dom_node_inner_html(page_id(), node_id);
}

void ViewImplementation::get_dom_node_outer_html(Web::UniqueNodeID node_id)
{
    client().async_get_dom_node_outer_html(page_id(), node_id);
}

void ViewImplementation::set_dom_node_outer_html(Web::UniqueNodeID node_id, String const& html)
{
    client().async_set_dom_node_outer_html(page_id(), node_id, html);
}

void ViewImplementation::set_dom_node_text(Web::UniqueNodeID node_id, String const& text)
{
    client().async_set_dom_node_text(page_id(), node_id, text);
}

void ViewImplementation::set_dom_node_tag(Web::UniqueNodeID node_id, String const& name)
{
    client().async_set_dom_node_tag(page_id(), node_id, name);
}

void ViewImplementation::add_dom_node_attributes(Web::UniqueNodeID node_id, ReadonlySpan<Attribute> attributes)
{
    client().async_add_dom_node_attributes(page_id(), node_id, attributes);
}

void ViewImplementation::replace_dom_node_attribute(Web::UniqueNodeID node_id, String const& name, ReadonlySpan<Attribute> replacement_attributes)
{
    client().async_replace_dom_node_attribute(page_id(), node_id, name, replacement_attributes);
}

void ViewImplementation::create_child_element(Web::UniqueNodeID node_id)
{
    client().async_create_child_element(page_id(), node_id);
}

void ViewImplementation::create_child_text_node(Web::UniqueNodeID node_id)
{
    client().async_create_child_text_node(page_id(), node_id);
}

void ViewImplementation::insert_dom_node_before(Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id)
{
    client().async_insert_dom_node_before(page_id(), node_id, parent_node_id, sibling_node_id);
}

void ViewImplementation::clone_dom_node(Web::UniqueNodeID node_id)
{
    client().async_clone_dom_node(page_id(), node_id);
}

void ViewImplementation::remove_dom_node(Web::UniqueNodeID node_id)
{
    client().async_remove_dom_node(page_id(), node_id);
}

void ViewImplementation::list_style_sheets()
{
    client().async_list_style_sheets(page_id());
}

void ViewImplementation::request_style_sheet_source(Web::CSS::StyleSheetIdentifier const& identifier)
{
    client().async_request_style_sheet_source(page_id(), identifier);
}

void ViewImplementation::debug_request(ByteString const& request, ByteString const& argument)
{
    client().async_debug_request(page_id(), request, argument);
}

void ViewImplementation::run_javascript(String const& js_source)
{
    client().async_run_javascript(page_id(), js_source);
}

void ViewImplementation::js_console_input(String const& js_source)
{
    client().async_js_console_input(page_id(), js_source);
}

void ViewImplementation::exit_fullscreen()
{
    client().async_exit_fullscreen(page_id());
}

void ViewImplementation::alert_closed()
{
    client().async_alert_closed(page_id());
}

void ViewImplementation::confirm_closed(bool accepted)
{
    client().async_confirm_closed(page_id(), accepted);
}

void ViewImplementation::prompt_closed(Optional<String> const& response)
{
    client().async_prompt_closed(page_id(), response);
}

void ViewImplementation::color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    client().async_color_picker_update(page_id(), picked_color, state);
}

void ViewImplementation::file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files)
{
    client().async_file_picker_closed(page_id(), move(selected_files));
}

void ViewImplementation::select_dropdown_closed(Optional<u32> const& selected_item_id)
{
    client().async_select_dropdown_closed(page_id(), selected_item_id);
}

void ViewImplementation::paste_text_from_clipboard()
{
    client().async_paste(page_id(), Application::the().clipboard_text());
}

void ViewImplementation::retrieved_clipboard_entries(u64 request_id, ReadonlySpan<Web::Clipboard::SystemClipboardItem> items)
{
    client().async_retrieved_clipboard_entries(page_id(), request_id, items);
}

void ViewImplementation::toggle_page_mute_state()
{
    m_mute_state = Web::HTML::invert_mute_state(m_mute_state);
    client().async_toggle_page_mute_state(page_id());
}

void ViewImplementation::did_change_audio_play_state(Badge<WebContentClient>, Web::HTML::AudioPlayState play_state)
{
    bool state_changed = false;

    switch (play_state) {
    case Web::HTML::AudioPlayState::Paused:
        if (--m_number_of_elements_playing_audio == 0) {
            m_audio_play_state = play_state;
            state_changed = true;
        }
        break;

    case Web::HTML::AudioPlayState::Playing:
        if (m_number_of_elements_playing_audio++ == 0) {
            m_audio_play_state = play_state;
            state_changed = true;
        }
        break;
    }

    if (state_changed && on_audio_play_state_changed)
        on_audio_play_state_changed(m_audio_play_state);
}

void ViewImplementation::did_update_session_history(Badge<WebContentClient>, String traversable_navigable_id, i32 current_step, Vector<SerializedSessionHistoryEntry> entries)
{
    m_traversable_navigable_id = move(traversable_navigable_id);
    m_session_history_current_step = current_step;
    m_session_history_entries = move(entries);

    auto all_steps = get_all_used_history_steps(m_session_history_entries);
    auto current_index = all_steps.find_first_index(m_session_history_current_step);
    bool back_enabled = current_index.has_value() && *current_index > 0;
    bool forward_enabled = current_index.has_value() && *current_index + 1 < all_steps.size();
    m_navigate_back_action->set_enabled(back_enabled);
    m_navigate_forward_action->set_enabled(forward_enabled);
}

void ViewImplementation::did_request_traversal_by_delta(Badge<WebContentClient>, i32 delta, Optional<u64> source_snapshot_and_initiator_id, Web::HTML::UserNavigationInvolvement user_involvement)
{
    auto all_steps = get_all_used_history_steps(m_session_history_entries);
    auto current_index = all_steps.find_first_index(m_session_history_current_step);
    if (!current_index.has_value())
        return;
    auto target_index = static_cast<int>(*current_index) + delta;
    if (target_index < 0 || target_index >= static_cast<int>(all_steps.size()))
        return;

    TraversalCommand cmd;
    cmd.target_step = all_steps[target_index];
    cmd.check_for_cancelation = true;
    cmd.navigation_type = Web::Bindings::NavigationType::Traverse;
    cmd.user_involvement = user_involvement;
    cmd.source_snapshot_and_initiator_id = source_snapshot_and_initiator_id;
    m_session_history_traversal_queue.append(SessionHistoryCommand { cmd });
    process_next_session_history_command();
}

void ViewImplementation::did_request_session_history_operation(Badge<WebContentClient>, u64 operation_id)
{
    AsyncOperationCommand cmd;
    cmd.operation_id = operation_id;
    m_session_history_traversal_queue.append(SessionHistoryCommand { cmd });
    process_next_session_history_command();
}

void ViewImplementation::did_request_session_history_prep(Badge<WebContentClient>, u64 prep_id)
{
    PrepAndApplyCommand cmd;
    cmd.prep_operation_id = prep_id;
    m_session_history_traversal_queue.append(SessionHistoryCommand { cmd });
    process_next_session_history_command();
}

void ViewImplementation::did_request_session_history_sync_navigation(Badge<WebContentClient>, u64 prep_id, String target_navigable_id)
{
    // Queue-jumping during traversal: if an active traversal is in progress and the target navigable
    // is not in the exclusion set (i.e., not yet processed), execute the sync nav immediately as a
    // queue-jump (spec step 14.1 of "apply the history step").
    if (m_active_traversal.has_value() && !m_running_nested_queue_jump && !m_traversal_exclusion_set.contains(target_navigable_id)) {
        m_running_nested_queue_jump = true;
        client().async_execute_session_history_prep(page_id(), prep_id);
        return;
    }

    SynchronousNavigationCommand cmd;
    cmd.prep_operation_id = prep_id;
    cmd.target_navigable_id = move(target_navigable_id);
    m_session_history_traversal_queue.append(SessionHistoryCommand { move(cmd) });
    process_next_session_history_command();
}

void ViewImplementation::did_finish_session_history_operation(Badge<WebContentClient>)
{
    m_active_operation = false;
    process_next_session_history_command();
}

void ViewImplementation::process_next_session_history_command()
{
    if (m_active_traversal.has_value() || m_active_operation)
        return;

    auto command = m_session_history_traversal_queue.dequeue();
    if (!command.has_value())
        return;

    command->visit(
        [&](TraversalCommand& traversal) {
            // https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
            traversal.target_step = get_the_used_step(m_session_history_entries, traversal.target_step);

            // Pre-compute navigable classifications and history length/index.
            traversal.changing_navigable_ids = get_changing_navigable_ids(
                m_session_history_entries, m_traversable_navigable_id, m_session_history_current_step, traversal.target_step);
            traversal.non_changing_navigable_ids = get_non_changing_navigable_ids(
                m_session_history_entries, m_traversable_navigable_id, m_session_history_current_step, traversal.target_step);
            auto length_and_index = compute_script_history_length_and_index(
                m_session_history_entries, traversal.target_step);
            traversal.script_history_length = length_and_index.script_history_length;
            traversal.script_history_index = length_and_index.script_history_index;

            m_active_traversal = ActiveTraversalState { .command = traversal };

            // Phase B or CD: If unloading check is needed, start with Phase B; otherwise start Phase CD directly.
            if (traversal.check_for_cancelation) {
                client().async_traversal_check_if_unloading_is_canceled(
                    page_id(), traversal.target_step, traversal.source_snapshot_and_initiator_id, traversal.user_involvement);
            } else {
                start_traversal_processing();
            }
        },
        [&](SynchronousNavigationCommand& cmd) {
            // Same-document navigations use the prep-and-apply protocol, just like PrepAndApplyCommand.
            m_active_operation = true;
            client().async_execute_session_history_prep(page_id(), cmd.prep_operation_id);
        },
        [&](AsyncOperationCommand& cmd) {
            m_active_operation = true;
            client().async_execute_session_history_operation(page_id(), cmd.operation_id);
        },
        [&](PrepAndApplyCommand& cmd) {
            // Send the prep closure ID to WC for execution. WC will run the prep closure,
            // which does operation-specific work and then sends back either
            // did_finish_prep_for_history_step or did_finish_prep_no_history_step.
            m_active_operation = true;
            client().async_execute_session_history_prep(page_id(), cmd.prep_operation_id);
        });
}

void ViewImplementation::did_finish_prep_for_history_step(Badge<WebContentClient>, i32 target_step, bool check_for_cancelation, Optional<Web::Bindings::NavigationType> navigation_type, Web::HTML::UserNavigationInvolvement user_involvement, Optional<u64> source_snapshot_and_initiator_id, Optional<u64> cancel_callback_id)
{
    // The prep closure has finished. Now we have the parameters to drive the phase protocol,
    // just like a TraversalCommand. Build a TraversalCommand and start execution.
    TraversalCommand cmd;
    cmd.target_step = target_step;
    cmd.check_for_cancelation = check_for_cancelation;
    cmd.navigation_type = navigation_type;
    cmd.user_involvement = user_involvement;
    cmd.source_snapshot_and_initiator_id = source_snapshot_and_initiator_id;
    cmd.cancel_callback_id = cancel_callback_id;

    // Pre-compute navigable classifications and history length/index from serialized session history.
    cmd.target_step = get_the_used_step(m_session_history_entries, cmd.target_step);
    cmd.changing_navigable_ids = get_changing_navigable_ids(
        m_session_history_entries, m_traversable_navigable_id, m_session_history_current_step, cmd.target_step);
    cmd.non_changing_navigable_ids = get_non_changing_navigable_ids(
        m_session_history_entries, m_traversable_navigable_id, m_session_history_current_step, cmd.target_step);
    auto length_and_index = compute_script_history_length_and_index(
        m_session_history_entries, cmd.target_step);
    cmd.script_history_length = length_and_index.script_history_length;
    cmd.script_history_index = length_and_index.script_history_index;

    // Queue-jump mode: this prep came from a SynchronousNavigationCommand during an active traversal.
    // Store as the inner (queue-jump) traversal and drive phases for it.
    if (m_running_nested_queue_jump) {
        m_queue_jump_traversal = ActiveTraversalState { .command = cmd };

        // Same-doc navigations never need cancelation checks, so go straight to Phase CD.
        VERIFY(!cmd.check_for_cancelation);
        start_traversal_processing();
        return;
    }

    // Normal mode: this is a new outer traversal from a PrepAndApplyCommand or SynchronousNavigationCommand.
    m_active_traversal = ActiveTraversalState { .command = cmd };

    // Phase B or CD: If unloading check is needed, start with Phase B; otherwise start Phase CD directly.
    if (cmd.check_for_cancelation) {
        client().async_traversal_check_if_unloading_is_canceled(
            page_id(), cmd.target_step, cmd.source_snapshot_and_initiator_id, cmd.user_involvement);
    } else {
        start_traversal_processing();
    }
}

void ViewImplementation::did_finish_prep_no_history_step(Badge<WebContentClient>)
{
    // The prep closure determined no history step is needed (e.g., document was null).

    // Queue-jump mode: the same-doc nav was a no-op (e.g., navigable was destroyed).
    // Check for more queue-jumps or resume the outer traversal.
    if (m_running_nested_queue_jump) {
        m_running_nested_queue_jump = false;

        auto next_jump = m_session_history_traversal_queue.take_first_synchronous_navigation_not_targeting(m_traversal_exclusion_set);
        if (next_jump.has_value()) {
            m_running_nested_queue_jump = true;
            client().async_execute_session_history_prep(page_id(), next_jump->prep_operation_id);
            return;
        }

        // No more queue-jumps. Resume outer traversal.
        if (!m_active_traversal->processing_navigable)
            process_next_traversal_step();
        return;
    }

    // Normal mode: clear the active operation flag and process the next command.
    m_active_operation = false;
    process_next_session_history_command();
}

void ViewImplementation::did_finish_traversal_unloading_check(Badge<WebContentClient>, TraversalUnloadingCheckResult result)
{
    if (!m_active_traversal.has_value())
        return;

    if (result != TraversalUnloadingCheckResult::Continue) {
        // Traversal was canceled. If there's a cancel callback (from Navigation::traverseTo),
        // notify WC so it can reject the Navigation API promise.
        if (m_active_traversal->command.cancel_callback_id.has_value()) {
            client().async_traversal_canceled(page_id(), *m_active_traversal->command.cancel_callback_id, result);
        }

        m_active_traversal = {};
        process_next_session_history_command();
        return;
    }

    // Phase CD: Start iterative per-navigable processing.
    start_traversal_processing();
}

void ViewImplementation::did_finish_traversal_navigable(Badge<WebContentClient>, String navigable_id)
{
    (void)navigable_id;
    if (!current_traversal_state().has_value())
        return;

    current_traversal_state()->processing_navigable = false;

    // If a nested queue-jump is still running, wait for it to finish before advancing.
    if (m_running_nested_queue_jump)
        return;

    process_next_traversal_step();
}

Optional<ViewImplementation::ActiveTraversalState>& ViewImplementation::current_traversal_state()
{
    if (m_queue_jump_traversal.has_value())
        return m_queue_jump_traversal;
    return m_active_traversal;
}

void ViewImplementation::start_traversal_processing()
{
    VERIFY(current_traversal_state().has_value());

    auto& state = current_traversal_state().value();

    // Reset iterative state for Phase CD.
    state.navigable_index = 0;
    // Only clear the exclusion set for the outer traversal, not for queue-jumps.
    if (!m_queue_jump_traversal.has_value()) {
        m_traversal_exclusion_set.clear();
        m_running_nested_queue_jump = false;
    }
    state.processing_navigable = false;

    auto& cmd = state.command;

    // Send setup message for spec step 8 (set current session history entry + ongoing navigation
    // for all changing navigables). IPC ordering guarantees this is processed before the first
    // traversal_process_navigable message.
    client().async_traversal_setup_changing_navigables(page_id(), cmd.target_step, cmd.changing_navigable_ids);

    // Start iterative per-navigable processing.
    process_next_traversal_step();
}

void ViewImplementation::process_next_traversal_step()
{
    if (!current_traversal_state().has_value())
        return;

    auto& state = current_traversal_state().value();
    auto& cmd = state.command;

    // Step 14.1: Check for synchronous navigation queue-jumps.
    // Only check for queue-jumps from the outer traversal (not during an inner queue-jump traversal).
    if (!m_queue_jump_traversal.has_value()) {
        auto queue_jump = m_session_history_traversal_queue.take_first_synchronous_navigation_not_targeting(m_traversal_exclusion_set);
        if (queue_jump.has_value()) {
            m_running_nested_queue_jump = true;
            client().async_execute_session_history_prep(page_id(), queue_jump->prep_operation_id);
            return;
        }
    }

    // Process the next navigable.
    if (state.navigable_index < cmd.changing_navigable_ids.size()) {
        auto const& navigable_id = cmd.changing_navigable_ids[state.navigable_index];
        state.navigable_index++;

        // Step 14.8: Add to the exclusion set.
        m_traversal_exclusion_set.set(navigable_id);

        // Recompute script history length/index (they don't change between navigables in current
        // implementation, but this is where the spec says to compute them).
        state.processing_navigable = true;
        client().async_traversal_process_navigable(
            page_id(), navigable_id, cmd.target_step,
            cmd.source_snapshot_and_initiator_id, cmd.user_involvement, cmd.navigation_type,
            cmd.script_history_length, cmd.script_history_index);
        return;
    }

    // All changing navigables processed. Phase E: Update non-changing navigables.
    client().async_traversal_update_non_changing_navigables(
        page_id(), cmd.non_changing_navigable_ids,
        cmd.script_history_length, cmd.script_history_index);
}

void ViewImplementation::did_finish_traversal_non_changing_update(Badge<WebContentClient>)
{
    if (!current_traversal_state().has_value())
        return;

    // Phase F (finalization): Update UI-side state.
    auto target_step = current_traversal_state()->command.target_step;
    m_session_history_current_step = target_step;

    // Update the URL bar from the traversable's top-level entry at the target step.
    for (auto const& entry : m_session_history_entries) {
        if (entry.step == target_step) {
            m_url = entry.url;
            if (on_url_change)
                on_url_change(entry.url);
            break;
        }
    }

    auto all_steps = get_all_used_history_steps(m_session_history_entries);
    auto current_index = all_steps.find_first_index(m_session_history_current_step);
    bool back_enabled = current_index.has_value() && *current_index > 0;
    bool forward_enabled = current_index.has_value() && *current_index + 1 < all_steps.size();
    m_navigate_back_action->set_enabled(back_enabled);
    m_navigate_forward_action->set_enabled(forward_enabled);

    // Queue-jump mode: finalize the inner traversal, check for more queue-jumps, resume outer.
    if (m_queue_jump_traversal.has_value()) {
        m_queue_jump_traversal = {};
        m_running_nested_queue_jump = false;

        // Check for more queue-jumps (spec step 14.1 — check on every iteration).
        auto next_jump = m_session_history_traversal_queue.take_first_synchronous_navigation_not_targeting(m_traversal_exclusion_set);
        if (next_jump.has_value()) {
            m_running_nested_queue_jump = true;
            client().async_execute_session_history_prep(page_id(), next_jump->prep_operation_id);
            return;
        }

        // No more queue-jumps. Resume the outer traversal.
        if (!m_active_traversal->processing_navigable)
            process_next_traversal_step();
        return;
    }

    // Normal mode: outer traversal complete.
    m_active_traversal = {};
    process_next_session_history_command();
}

void ViewImplementation::did_allocate_backing_stores(Badge<WebContentClient>, i32 front_bitmap_id, Gfx::ShareableBitmap const& front_bitmap, i32 back_bitmap_id, Gfx::ShareableBitmap const& back_bitmap)
{
    if (m_client_state.has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_bitmap = m_client_state.front_bitmap.bitmap;
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }
    m_client_state.has_usable_bitmap = false;

    m_client_state.front_bitmap.bitmap = front_bitmap.bitmap();
    m_client_state.front_bitmap.id = front_bitmap_id;
    m_client_state.back_bitmap.bitmap = back_bitmap.bitmap();
    m_client_state.back_bitmap.id = back_bitmap_id;
}

#ifdef AK_OS_MACOS
void ViewImplementation::did_allocate_iosurface_backing_stores(i32 front_id, Core::MachPort&& front_port, i32 back_id, Core::MachPort&& back_port)
{
    if (m_client_state.has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_bitmap = m_client_state.front_bitmap.bitmap;
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }
    m_client_state.has_usable_bitmap = false;

    auto front_iosurface = Core::IOSurfaceHandle::from_mach_port(move(front_port));
    auto back_iosurface = Core::IOSurfaceHandle::from_mach_port(move(back_port));

    auto front_size = Gfx::IntSize { front_iosurface.width(), front_iosurface.height() };
    auto back_size = Gfx::IntSize { back_iosurface.width(), back_iosurface.height() };

    auto bytes_per_row = front_iosurface.bytes_per_row();

    auto* front_ref = front_iosurface.core_foundation_pointer();
    auto* back_ref = back_iosurface.core_foundation_pointer();

    auto front_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, front_size, bytes_per_row, front_iosurface.data(), [handle = move(front_iosurface)] { });
    auto back_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, back_size, bytes_per_row, back_iosurface.data(), [handle = move(back_iosurface)] { });

    m_client_state.front_bitmap.bitmap = front_bitmap.release_value_but_fixme_should_propagate_errors();
    m_client_state.front_bitmap.id = front_id;
    m_client_state.front_bitmap.iosurface_ref = front_ref;
    m_client_state.back_bitmap.bitmap = back_bitmap.release_value_but_fixme_should_propagate_errors();
    m_client_state.back_bitmap.id = back_id;
    m_client_state.back_bitmap.iosurface_ref = back_ref;
}
#endif

void ViewImplementation::update_zoom()
{
    if (m_zoom_level != 1.0) {
        m_reset_zoom_action->set_text(MUST(String::formatted("{}%", round_to<int>(m_zoom_level * 100))));
        m_reset_zoom_action->set_visible(true);
    } else {
        m_reset_zoom_action->set_visible(false);
    }

    client().async_set_zoom_level(m_client_state.page_index, m_zoom_level);
}

void ViewImplementation::handle_resize()
{
    client().async_set_viewport(page_id(), this->viewport_size(), m_device_pixel_ratio);
}

void ViewImplementation::initialize_client(CreateNewClient create_new_client)
{
    if (create_new_client == CreateNewClient::Yes) {
        m_client_state = {};

        // FIXME: Fail to open the tab, rather than crashing the whole application if this fails.
        m_client_state.client = Application::the().launch_web_content_process(*this).release_value_but_fixme_should_propagate_errors();
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    m_client_state.client_handle = MUST(Web::Crypto::generate_random_uuid());
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);
    client().async_set_zoom_level(m_client_state.page_index, m_zoom_level);
    client().async_set_viewport(m_client_state.page_index, viewport_size(), m_device_pixel_ratio);
    client().async_set_maximum_frames_per_second(m_client_state.page_index, m_maximum_frames_per_second);
    client().async_set_system_visibility_state(m_client_state.page_index, m_system_visibility_state);
    client().async_set_document_cookie_version_buffer(m_client_state.page_index, m_document_cookie_version_buffer);

    if (auto webdriver_content_ipc_path = Application::browser_options().webdriver_content_ipc_path; webdriver_content_ipc_path.has_value())
        client().async_connect_to_webdriver(m_client_state.page_index, *webdriver_content_ipc_path);

    Application::the().apply_view_options({}, *this);

    default_zoom_level_factor_changed();
    languages_changed();
    autoplay_settings_changed();
    global_privacy_control_changed();

    // If DevTools is connected, notify the new WebContent process.
    if (m_devtools_connected)
        client().async_did_connect_devtools_client(page_id());
}

void ViewImplementation::handle_web_content_process_crash(LoadErrorPage load_error_page)
{
    auto const headless_mode = Application::browser_options().headless_mode.has_value();

    if (!headless_mode) {
        dbgln("\033[31;1mWebContent process crashed!\033[0m Last page loaded: {}", m_url);
        dbgln("Consider raising an issue at https://github.com/LadybirdBrowser/ladybird/issues/new/choose");
    }

    ++m_crash_count;
    constexpr size_t max_reasonable_crash_count = 5U;
    if (m_crash_count >= max_reasonable_crash_count) {
        if (!headless_mode) {
            dbgln("WebContent has crashed {} times in quick succession! Not restarting...", m_crash_count);
            m_repeated_crash_timer->stop();
            return;
        }
        // In headless mode, always respawn - tests need a working WebContent for each test.
        // Reset the crash count so we can continue running tests.
        m_crash_count = 0;
    }
    m_repeated_crash_timer->restart();

    // In headless mode, respawn WebContent but skip the error page.
    if (headless_mode)
        load_error_page = LoadErrorPage::No;

    initialize_client();
    VERIFY(m_client_state.client);

    // Clear session history traversal state — the old WebContent process is gone, so any
    // in-flight traversal or operation will never complete. Without this, the queue guard
    // (m_active_traversal / m_active_operation) permanently blocks all future navigation.
    m_active_traversal = {};
    m_queue_jump_traversal = {};
    m_active_operation = false;
    m_session_history_traversal_queue.clear();
    m_traversal_exclusion_set.clear();
    m_running_nested_queue_jump = false;

    // Don't keep a stale backup bitmap around.
    m_backup_bitmap = nullptr;

    handle_resize();

    if (load_error_page == LoadErrorPage::Yes) {
        StringBuilder builder;
        builder.append("<!DOCTYPE html>"sv);
        builder.append("<html lang=\"en\"><head><meta charset=\"UTF-8\"><title>Error!</title><style>"
                       ":root { color-scheme: light dark; font-family: system-ui, sans-serif; }"
                       "body { display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; box-sizing: border-box; margin: 0; padding: 1rem; text-align: center; }"
                       "header { display: flex; flex-direction: column; align-items: center; gap: 2rem; margin-bottom: 1rem; }"
                       "svg { height: 64px; width: auto; stroke: currentColor; fill: none; stroke-width: 1.5; stroke-linecap: round; stroke-linejoin: round; }"
                       "h1 { margin: 0; font-size: 1.5rem; }"
                       "p { font-size: 1rem; color: #555; }"
                       "</style></head><body>"sv);
        builder.append("<header>"sv);
        builder.append("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 17.5 21.5\">"sv);
        builder.append("<path class=\"b\" d=\"M11.75.75h-9c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2v-13l-5-5z\"/>"sv);
        builder.append("<path class=\"b\" d=\"M10.75.75v4c0 1.1.9 2 2 2h4M4.75 9.75l2 2M10.75 9.75l2 2M12.75 9.75l-2 2M6.75 9.75l-2 2M5.75 16.75c1-2.67 5-2.67 6 0\"/></svg>"sv);
        auto escaped_url = escape_html_entities(m_url.to_byte_string());
        builder.append("<h1>Ladybird flew off-course!</h1>"sv);
        builder.appendff("<p>The web page <a href=\"{}\">{}</a> has crashed.<br><br>You can reload the page to try again.</p>", escaped_url, escaped_url);
        builder.append("</body></html>"sv);
        load_html(builder.to_byte_string());
    }
}

void ViewImplementation::default_zoom_level_factor_changed()
{
    auto const default_zoom_level_factor = Application::settings().default_zoom_level_factor();
    set_zoom(default_zoom_level_factor);
}

void ViewImplementation::languages_changed()
{
    auto const& languages = Application::settings().languages();
    client().async_set_preferred_languages(page_id(), languages);
}

void ViewImplementation::autoplay_settings_changed()
{
    auto const& autoplay_settings = Application::settings().autoplay_settings();
    auto const& web_content_options = Application::web_content_options();

    if (autoplay_settings.enabled_globally || web_content_options.enable_autoplay == EnableAutoplay::Yes)
        client().async_set_autoplay_allowed_on_all_websites(page_id());
    else
        client().async_set_autoplay_allowlist(page_id(), autoplay_settings.site_filters.values());
}

void ViewImplementation::global_privacy_control_changed()
{
    auto global_privacy_control = Application::settings().global_privacy_control();
    client().async_set_enable_global_privacy_control(page_id(), global_privacy_control == GlobalPrivacyControl::Yes);
}

static ErrorOr<LexicalPath> save_screenshot(Gfx::Bitmap const* bitmap)
{
    if (!bitmap)
        return Error::from_string_literal("Failed to take a screenshot");

    auto file = AK::UnixDateTime::now().to_byte_string("screenshot-%Y-%m-%d-%H-%M-%S.png"sv);
    auto path = TRY(Application::the().path_for_downloaded_file(file));

    auto encoded = TRY(Gfx::PNGWriter::encode(*bitmap));

    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted(encoded));

    return path;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_screenshot(ScreenshotType type)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicity, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    switch (type) {
    case ScreenshotType::Visible:
        if (auto* visible_bitmap = m_client_state.has_usable_bitmap ? m_client_state.front_bitmap.bitmap.ptr() : m_backup_bitmap.ptr()) {
            if (auto result = save_screenshot(visible_bitmap); result.is_error())
                promise->reject(result.release_error());
            else
                promise->resolve(result.release_value());
        }
        break;

    case ScreenshotType::Full:
        m_pending_screenshot = promise;
        client().async_take_document_screenshot(page_id());
        break;
    }

    return promise;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_dom_node_screenshot(Web::UniqueNodeID node_id)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicity, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    m_pending_screenshot = promise;
    client().async_take_dom_node_screenshot(page_id(), node_id);

    return promise;
}

void ViewImplementation::did_receive_screenshot(Badge<WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    VERIFY(m_pending_screenshot);

    if (auto result = save_screenshot(screenshot.bitmap()); result.is_error())
        m_pending_screenshot->reject(result.release_error());
    else
        m_pending_screenshot->resolve(result.release_value());

    m_pending_screenshot = nullptr;
}

NonnullRefPtr<Core::Promise<String>> ViewImplementation::request_internal_page_info(PageInfoType type)
{
    auto promise = Core::Promise<String>::construct();

    if (m_pending_info_request) {
        // For simplicity, only allow one info request at a time for now.
        promise->reject(Error::from_string_literal("A page info request is already in progress"));
        return promise;
    }

    m_pending_info_request = promise;
    client().async_request_internal_page_info(page_id(), type);

    return promise;
}

void ViewImplementation::did_receive_internal_page_info(Badge<WebContentClient>, PageInfoType, Optional<Core::AnonymousBuffer> const& info)
{
    VERIFY(m_pending_info_request);

    String info_string;
    if (!info.has_value()) {
        info_string = "(no page)"_string;
    } else {
        info_string = MUST(String::from_utf8(info->bytes()));
    }
    m_pending_info_request->resolve(move(info_string));
    m_pending_info_request = nullptr;
}

ErrorOr<LexicalPath> ViewImplementation::dump_gc_graph()
{
    auto promise = request_internal_page_info(PageInfoType::GCGraph);
    auto gc_graph_json = TRY(promise->await());

    LexicalPath path { Core::StandardPaths::tempfile_directory() };
    path = path.append(TRY(AK::UnixDateTime::now().to_string("gc-graph-%Y-%m-%d-%H-%M-%S.js"sv)));

    // Write as a .js file so gc-heap-explorer.html can load it via <script> tag (avoiding CORS issues with file:// URLs)
    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted("var GC_GRAPH_DUMP = "sv.bytes()));
    TRY(dump_file->write_until_depleted(gc_graph_json.bytes()));
    TRY(dump_file->write_until_depleted(";\n"sv.bytes()));

    return path;
}

void ViewImplementation::set_user_style_sheet(String const& source)
{
    client().async_set_user_style(page_id(), source);
}

void ViewImplementation::use_native_user_style_sheet()
{
    extern String native_stylesheet_source;
    set_user_style_sheet(native_stylesheet_source);
}

void ViewImplementation::initialize_context_menus()
{
    auto& application = Application::the();

    m_navigate_back_action = Action::create("Go Back"sv, ActionID::NavigateBack, [this]() {
        traverse_the_history_by_delta(-1);
    });
    m_navigate_forward_action = Action::create("Go Forward"sv, ActionID::NavigateForward, [this]() {
        traverse_the_history_by_delta(+1);
    });
    m_navigate_back_action->set_enabled(false);
    m_navigate_forward_action->set_enabled(false);

    m_reset_zoom_action = Action::create("100%"sv, ActionID::ResetZoomViaToolbar, [this]() {
        reset_zoom();
    });
    m_reset_zoom_action->set_tooltip("Reset zoom level"sv);
    m_reset_zoom_action->set_visible(false);

    m_search_selected_text_action = Action::create("Search Selected Text"sv, ActionID::SearchSelectedText, [this]() {
        auto const& search_engine = Application::settings().search_engine();
        if (!search_engine.has_value())
            return;

        auto url_string = search_engine->format_search_query_for_navigation(*m_search_text);
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes);
    });
    m_search_selected_text_action->set_visible(false);

    auto take_and_save_screenshot = [this](auto type) {
        take_screenshot(type)
            ->when_resolved([](auto const& path) {
                Application::the().display_download_confirmation_dialog("Screenshot"sv, path);
            })
            .when_rejected([](auto const& error) {
                if (error.is_errno() && error.code() == ECANCELED)
                    return;

                auto error_message = MUST(String::formatted("{}", error));
                Application::the().display_error_dialog(error_message);
            });
    };

    m_take_visible_screenshot_action = Action::create("Take Visible Screenshot"sv, ActionID::TakeVisibleScreenshot, [take_and_save_screenshot]() {
        take_and_save_screenshot(ScreenshotType::Visible);
    });
    m_take_full_screenshot_action = Action::create("Take Full Screenshot"sv, ActionID::TakeFullScreenshot, [take_and_save_screenshot]() {
        take_and_save_screenshot(ScreenshotType::Full);
    });

    m_open_in_new_tab_action = Action::create("Open in New Tab"sv, ActionID::OpenInNewTab, [this]() {
        Application::the().open_url_in_new_tab(m_context_menu_url, Web::HTML::ActivateTab::No);
    });
    m_copy_url_action = Action::create("Copy URL"sv, ActionID::CopyURL, [this]() {
        Application::the().insert_clipboard_entry({ url_text_to_copy(m_context_menu_url), "text/plain"_string });
    });

    m_open_image_action = Action::create("Open Image"sv, ActionID::OpenImage, [this]() {
        load(m_context_menu_url);
    });
    m_save_image_action = Action::create("Save Image As..."sv, ActionID::SaveImage, [this]() {
        auto download_path = Application::the().path_for_downloaded_file(m_context_menu_url.basename());
        if (download_path.is_error())
            return;

        Application::the().file_downloader().download_file(m_context_menu_url, download_path.release_value());
    });
    m_copy_image_action = Action::create("Copy Image"sv, ActionID::CopyImage, [this]() {
        if (!m_image_context_menu_bitmap.has_value())
            return;

        auto bitmap = m_image_context_menu_bitmap.release_value();
        if (!bitmap.is_valid())
            return;

        auto encoded = Gfx::PNGWriter::encode(*bitmap.bitmap());
        if (encoded.is_error())
            return;

        Application::the().insert_clipboard_entry({ ByteString { encoded.value().bytes() }, "image/png"_string });
    });

    m_open_audio_action = Action::create("Open Audio"sv, ActionID::OpenAudio, [this]() {
        load(m_context_menu_url);
    });
    m_open_video_action = Action::create("Open Video"sv, ActionID::OpenVideo, [this]() {
        load(m_context_menu_url);
    });
    m_media_play_action = Action::create("Play"sv, ActionID::PlayMedia, [this]() {
        client().async_toggle_media_play_state(page_id());
    });
    m_media_pause_action = Action::create("Pause"sv, ActionID::PauseMedia, [this]() {
        client().async_toggle_media_play_state(page_id());
    });
    m_media_mute_action = Action::create("Mute"sv, ActionID::MuteMedia, [this]() {
        client().async_toggle_media_mute_state(page_id());
    });
    m_media_unmute_action = Action::create("Unmute"sv, ActionID::UnmuteMedia, [this]() {
        client().async_toggle_media_mute_state(page_id());
    });
    m_media_show_controls_action = Action::create("Show Controls"sv, ActionID::ShowControls, [this]() {
        client().async_toggle_media_controls_state(page_id());
    });
    m_media_hide_controls_action = Action::create("Hide Controls"sv, ActionID::HideControls, [this]() {
        client().async_toggle_media_controls_state(page_id());
    });
    m_media_loop_action = Action::create_checkable("Loop"sv, ActionID::ToggleMediaLoopState, [this]() {
        client().async_toggle_media_loop_state(page_id());
    });
    m_media_enter_fullscreen_action = Action::create("Full Screen"sv, ActionID::EnterFullscreen, [this]() {
        client().async_toggle_media_fullscreen_state(page_id());
    });
    m_media_exit_fullscreen_action = Action::create("Exit Full Screen"sv, ActionID::ExitFullscreen, [this]() {
        client().async_toggle_media_fullscreen_state(page_id());
    });

    m_page_context_menu = Menu::create("Page Context Menu"sv);
    m_page_context_menu->add_action(*m_navigate_back_action);
    m_page_context_menu->add_action(*m_navigate_forward_action);
    m_page_context_menu->add_action(application.reload_action());
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(application.copy_selection_action());
    m_page_context_menu->add_action(application.paste_action());
    m_page_context_menu->add_action(application.select_all_action());
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_search_selected_text_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_take_visible_screenshot_action);
    m_page_context_menu->add_action(*m_take_full_screenshot_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(application.view_source_action());

    m_link_context_menu = Menu::create("Link Context Menu"sv);
    m_link_context_menu->add_action(*m_open_in_new_tab_action);
    m_link_context_menu->add_action(*m_copy_url_action);

    m_image_context_menu = Menu::create("Image Context Menu"sv);
    m_image_context_menu->add_action(*m_open_image_action);
    m_image_context_menu->add_action(*m_open_in_new_tab_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_save_image_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_copy_image_action);
    m_image_context_menu->add_action(*m_copy_url_action);

    m_media_context_menu = Menu::create("Media Context Menu"sv);
    m_media_context_menu->add_action(*m_media_play_action);
    m_media_context_menu->add_action(*m_media_pause_action);
    m_media_context_menu->add_action(*m_media_mute_action);
    m_media_context_menu->add_action(*m_media_unmute_action);
    m_media_context_menu->add_action(*m_media_show_controls_action);
    m_media_context_menu->add_action(*m_media_hide_controls_action);
    m_media_context_menu->add_action(*m_media_loop_action);
    m_media_context_menu->add_action(*m_media_enter_fullscreen_action);
    m_media_context_menu->add_action(*m_media_exit_fullscreen_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_open_audio_action);
    m_media_context_menu->add_action(*m_open_video_action);
    m_media_context_menu->add_action(*m_open_in_new_tab_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_copy_url_action);
}

void ViewImplementation::did_request_page_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position)
{
    auto const& search_engine = Application::settings().search_engine();

    auto selected_text = search_engine.has_value() ? selected_text_with_whitespace_collapsed() : OptionalNone {};
    TemporaryChange change_url { m_search_text, move(selected_text) };

    if (m_search_text.has_value()) {
        m_search_selected_text_action->set_text(search_engine->format_search_query_for_display(*m_search_text));
        m_search_selected_text_action->set_visible(true);
    } else {
        m_search_selected_text_action->set_visible(false);
    }

    if (m_page_context_menu->on_activation)
        m_page_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_link_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url)
{
    m_context_menu_url = move(url);

    m_open_in_new_tab_action->set_text("Open in New Tab"sv);

    switch (url_type(m_context_menu_url)) {
    case URLType::Email:
        m_copy_url_action->set_text("Copy Email Address"sv);
        break;
    case URLType::Telephone:
        m_copy_url_action->set_text("Copy Phone Number"sv);
        break;
    case URLType::Other:
        m_copy_url_action->set_text("Copy Link Address"sv);
        break;
    }

    if (m_link_context_menu->on_activation)
        m_link_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_image_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url, Optional<Gfx::ShareableBitmap> bitmap)
{
    m_context_menu_url = move(url);
    m_image_context_menu_bitmap = move(bitmap);

    m_open_in_new_tab_action->set_text("Open Image in New Tab"sv);
    m_copy_url_action->set_text("Copy Image URL"sv);

    m_copy_image_action->set_enabled(m_image_context_menu_bitmap.has_value());

    if (m_image_context_menu->on_activation)
        m_image_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_media_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, Web::Page::MediaContextMenu menu)
{
    m_context_menu_url = move(menu.media_url);

    m_open_in_new_tab_action->set_text(menu.is_video ? "Open Video in New Tab"sv : "Open Audio in new Tab"sv);
    m_copy_url_action->set_text(menu.is_video ? "Copy Video URL"sv : "Copy Audio URL"sv);

    m_open_audio_action->set_visible(!menu.is_video);
    m_open_video_action->set_visible(menu.is_video);

    m_media_play_action->set_visible(!menu.is_playing);
    m_media_pause_action->set_visible(menu.is_playing);

    m_media_mute_action->set_visible(!menu.is_muted);
    m_media_unmute_action->set_visible(menu.is_muted);

    m_media_show_controls_action->set_visible(!menu.has_user_agent_controls);
    m_media_hide_controls_action->set_visible(menu.has_user_agent_controls);

    m_media_loop_action->set_checked(menu.is_looping);

    m_media_enter_fullscreen_action->set_visible(menu.is_video && !menu.is_fullscreen);
    m_media_exit_fullscreen_action->set_visible(menu.is_video && menu.is_fullscreen);

    if (m_media_context_menu->on_activation)
        m_media_context_menu->on_activation(to_widget_position(content_position));
}

u64 ViewImplementation::add_navigation_listener(NavigationListener listener)
{
    auto id = m_next_navigation_listener_id++;
    m_navigation_listeners.set(id, move(listener));
    return id;
}

void ViewImplementation::remove_navigation_listener(u64 listener_id)
{
    m_navigation_listeners.remove(listener_id);
}

void ViewImplementation::request_close()
{
    client().async_request_close(page_id());
}

}
