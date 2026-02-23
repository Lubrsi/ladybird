/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWebView/SessionHistoryEntryData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TraversableNavigable);

TraversableNavigable::TraversableNavigable(GC::Ref<Page> page)
    : Navigable(page, page->client().is_svg_page_client())
    , m_storage_shed(StorageAPI::StorageShed::create(page->heap()))
{
}

TraversableNavigable::~TraversableNavigable() = default;

void TraversableNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_emulated_position_data.has<GC::Ref<Geolocation::GeolocationCoordinates>>())
        visitor.visit(m_emulated_position_data.get<GC::Ref<Geolocation::GeolocationCoordinates>>());
    visitor.visit(m_session_history_entries);
    visitor.visit(m_storage_shed);
    for (auto& [_, entry] : m_source_snapshot_map) {
        visitor.visit(entry.source_snapshot_params);
        visitor.visit(entry.initiator);
    }
    for (auto& [_, closure] : m_operation_map)
        visitor.visit(closure);
    for (auto& [_, closure] : m_prep_map)
        visitor.visit(closure);
    for (auto& [_, callback] : m_cancel_callback_map)
        visitor.visit(callback);
}

void TraversableNavigable::push_session_history_to_ui()
{
    HashMap<DocumentState const*, u64> document_id_map;
    Vector<WebView::SerializedSessionHistoryEntry> serialized_entries;
    serialized_entries.ensure_capacity(m_session_history_entries.size());
    for (auto const& entry : m_session_history_entries)
        serialized_entries.unchecked_append(entry->serialize(document_id_map));
    page().client().page_did_update_session_history(id(), m_current_session_history_step, move(serialized_entries));
}

u64 TraversableNavigable::store_source_snapshot_and_initiator(GC::Ptr<SourceSnapshotParams> source_snapshot_params, GC::Ptr<Navigable> initiator)
{
    auto id = m_next_source_snapshot_id++;
    m_source_snapshot_map.set(id, { source_snapshot_params, initiator });
    return id;
}

Optional<TraversableNavigable::SourceSnapshotAndInitiator> TraversableNavigable::take_source_snapshot_and_initiator(u64 id)
{
    return m_source_snapshot_map.take(id);
}

Optional<TraversableNavigable::SourceSnapshotAndInitiator> TraversableNavigable::get_source_snapshot_and_initiator(u64 id) const
{
    auto it = m_source_snapshot_map.find(id);
    if (it == m_source_snapshot_map.end())
        return {};
    return it->value;
}

u64 TraversableNavigable::store_session_history_operation(GC::Ref<GC::Function<NonnullRefPtr<Core::Promise<Empty>>()>> closure)
{
    auto id = m_next_operation_id++;
    m_operation_map.set(id, closure);
    return id;
}

GC::Ptr<GC::Function<NonnullRefPtr<Core::Promise<Empty>>()>> TraversableNavigable::take_session_history_operation(u64 id)
{
    auto closure = m_operation_map.take(id);
    if (closure.has_value())
        return closure.value();
    return {};
}

void TraversableNavigable::store_session_history_prep(GC::Ref<GC::Function<void()>> closure)
{
    auto id = m_next_prep_id++;
    m_prep_map.set(id, closure);
    page().client().page_did_request_session_history_prep(id);
}

u64 TraversableNavigable::store_session_history_prep_without_notify(GC::Ref<GC::Function<void()>> closure)
{
    auto id = m_next_prep_id++;
    m_prep_map.set(id, closure);
    return id;
}

GC::Ptr<GC::Function<void()>> TraversableNavigable::take_session_history_prep(u64 id)
{
    auto closure = m_prep_map.take(id);
    if (closure.has_value())
        return closure.value();
    return {};
}

u64 TraversableNavigable::store_cancel_callback(GC::Ref<GC::Function<void(HistoryStepResult)>> callback)
{
    auto id = m_next_cancel_callback_id++;
    m_cancel_callback_map.set(id, callback);
    return id;
}

void TraversableNavigable::run_cancel_callback(u64 id, HistoryStepResult reason)
{
    auto callback = m_cancel_callback_map.take(id);
    if (callback.has_value())
        callback.value()->function()(reason);
}

void TraversableNavigable::restore_session_history(i32 current_step, Vector<WebView::SerializedSessionHistoryEntry> entries)
{
    m_session_history_entries.clear();
    m_session_history_entries.ensure_capacity(entries.size());
    for (auto const& serialized_entry : entries)
        m_session_history_entries.unchecked_append(SessionHistoryEntry::create_from_serialized(heap(), serialized_entry));
    m_current_session_history_step = current_step;

    // The navigable's active/current session history entry was set during create_a_new_top_level_traversable
    // and points to the initial about:blank entry. After restoring, the entries vector contains new deserialized
    // objects, so the navigable's entry pointers are stale. Update the navigable to point at the restored entry
    // at the current step, transferring the about:blank document so active_document() remains valid.
    if (auto active = active_session_history_entry()) {
        for (auto& entry : m_session_history_entries) {
            if (entry->step().has<int>() && entry->step().get<int>() == current_step) {
                // Transfer the about:blank document to the restored entry so active_document() works.
                // The upcoming navigation will replace this entry's document anyway.
                if (active->document() && !entry->document())
                    entry->document_state()->set_document(active->document());

                // The initial about:blank document would force the next navigation to use "replace"
                // history handling (via navigation_must_be_a_replace), eating the previous entry.
                // Clear this flag so navigations push new entries onto the restored history.
                if (entry->document() && entry->document()->is_initial_about_blank())
                    entry->document()->set_is_initial_about_blank(false);

                set_active_session_history_entry(entry);
                set_current_session_history_entry(entry);
                break;
            }
        }
    }
}

static OrderedHashTable<TraversableNavigable*>& user_agent_top_level_traversable_set()
{
    static OrderedHashTable<TraversableNavigable*> set;
    return set;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page)
{
    // 1. Let group and document be the result of creating a new browsing context group and document.
    auto [group, document] = TRY(BrowsingContextGroup::create_a_new_browsing_context_group_and_document(page));

    // 2. Return group's browsing context set[0] and document.
    return BrowsingContextAndDocument { **group->browsing_context_set().begin(), document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
WebIDL::ExceptionOr<GC::Ref<TraversableNavigable>> TraversableNavigable::create_a_new_top_level_traversable(GC::Ref<Page> page, GC::Ptr<HTML::BrowsingContext> opener, String target_name)
{
    auto& vm = Bindings::main_thread_vm();

    // 1. Let document be null.
    GC::Ptr<DOM::Document> document = nullptr;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener) {
        document = TRY(create_a_new_top_level_browsing_context_and_document(page)).document;
    }

    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = TRY(BrowsingContext::create_a_new_auxiliary_browsing_context_and_document(page, *opener)).document;
    }

    // 4. Let documentState be a new document state, with
    auto document_state = vm.heap().allocate<DocumentState>();

    // document: document
    document_state->set_document(document);

    // initiator origin: null if opener is null; otherwise, document's origin
    document_state->set_initiator_origin(opener ? Optional<URL::Origin> {} : document->origin());

    // origin: document's origin
    document_state->set_origin(document->origin());

    // navigable target name: targetName
    document_state->set_navigable_target_name(target_name);

    // about base URL: document's about base URL
    document_state->set_about_base_url(document->about_base_url());

    // 5. Let traversable be a new traversable navigable.
    auto traversable = vm.heap().allocate<TraversableNavigable>(page);

    // 6. Initialize the navigable traversable given documentState.
    TRY_OR_THROW_OOM(vm, traversable->initialize_navigable(document_state, nullptr));

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->set_step(0);

    // 9. Append initialHistoryEntry to traversable's session history entries.
    traversable->m_session_history_entries.append(*initial_history_entry);
    traversable->set_has_session_history_entry_and_ready_for_navigation();

    traversable->push_session_history_to_ui();

    // FIXME: 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]

    // 11. Append traversable to the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().set(traversable);

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
WebIDL::ExceptionOr<GC::Ref<TraversableNavigable>> TraversableNavigable::create_a_fresh_top_level_traversable(GC::Ref<Page> page, URL::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> initial_navigation_post_resource)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = TRY(create_a_new_top_level_traversable(page, nullptr, {}));
    page->set_top_level_traversable(traversable);

    // AD-HOC: Set the default top-level emulated position data for the traversable, which points to Market St. SF.
    // FIXME: We should not emulate by default, but ask the user what to do. E.g. disable Geolocation, set an emulated
    //        position, or allow Ladybird to engage with the system's geolocation services. This is completely separate
    //        from the permission model for "powerful features" such as Geolocation.
    auto& realm = traversable->active_document()->realm();
    auto emulated_position_coordinates = realm.create<Geolocation::GeolocationCoordinates>(
        realm,
        Geolocation::CoordinatesData {
            .accuracy = 100.0,
            .latitude = 37.7647658,
            .longitude = -122.4345892,
            .altitude = 60.0,
            .altitude_accuracy = 10.0,
            .heading = 0.0,
            .speed = 0.0,
        });
    traversable->set_emulated_position_data(emulated_position_coordinates);

    // AD-HOC: Mark the about:blank document as finished parsing if we're only going to about:blank
    //         Skip the initial navigation as well. This matches the behavior of the window open steps.

    if (url_matches_about_blank(initial_navigation_url)) {
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(traversable->heap(), [traversable, initial_navigation_url] {
            // FIXME: We do this other places too when creating a new about:blank document. Perhaps it's worth a spec issue?
            HTML::HTMLParser::the_end(*traversable->active_document());

            // FIXME: If we perform the URL and history update steps here, we start hanging tests and the UI process will
            //        try to load() the initial URLs passed on the command line before we finish processing the events here.
            //        However, because we call this before the PageClient is fully initialized... that gets awkward.
        }));
    }

    else {
        // 2. Navigate traversable to initialNavigationURL using traversable's active document, with documentResource set to initialNavigationPostResource.
        TRY(traversable->navigate({ .url = initial_navigation_url,
            .source_document = *traversable->active_document(),
            .document_resource = initial_navigation_post_resource }));
    }

    // 3. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#top-level-traversable
bool TraversableNavigable::is_top_level_traversable() const
{
    // A top-level traversable is a traversable navigable with a null parent.
    return parent() == nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
Vector<int> TraversableNavigable::get_all_used_history_steps() const
{
    // FIXME: 1. Assert: this is running within traversable's session history traversal queue.

    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<int> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<GC::Ref<SessionHistoryEntry>>> entry_lists { session_history_entries() };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto entry_list = entry_lists.take_first();

        // 1. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. Append entry's step to steps.
            steps.set(entry->step().get<int>());

            // 2. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state()->nested_histories())
                entry_lists.append(nested_history.entries);
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-history-object-length-and-index
TraversableNavigable::HistoryObjectLengthAndIndex TraversableNavigable::get_the_history_object_length_and_index(int step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps();

    // 2. Let scriptHistoryLength be the size of steps.
    auto script_history_length = steps.size();

    // 3. Assert: steps contains step.
    VERIFY(steps.contains_slow(step));

    // 4. Let scriptHistoryIndex be the index of step in steps.
    auto script_history_index = *steps.find_first_index(step);

    // 5. Return (scriptHistoryLength, scriptHistoryIndex).
    return HistoryObjectLengthAndIndex {
        .script_history_length = script_history_length,
        .script_history_index = script_history_index
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-used-step
int TraversableNavigable::get_the_used_step(int step) const
{
    // 1. Let steps be the result of getting all used history steps within traversable.
    auto steps = get_all_used_history_steps();

    // 2. Return the greatest item in steps that is less than or equal to step.
    VERIFY(!steps.is_empty());
    Optional<int> result;
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i] <= step) {
            if (!result.has_value() || (result.value() < steps[i])) {
                result = steps[i];
            }
        }
    }
    return result.value();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#get-all-navigables-whose-current-session-history-entry-will-change-or-reload
Vector<GC::Root<Navigable>> TraversableNavigable::get_all_navigables_whose_current_session_history_entry_will_change_or_reload(int target_step) const
{
    // 1. Let results be an empty list.
    Vector<GC::Root<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<GC::Root<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry is not navigable's current session history entry or targetEntry's document state's reload
        //    pending is true, then append navigable to results.
        // AD-HOC: We don't want to choose a navigable that has ongoing traversal.
        if ((target_entry != navigable->current_session_history_entry() || target_entry->document_state()->reload_pending()) && !navigable->ongoing_navigation().has<Traversal>()) {
            results.append(*navigable);
        }

        // 3. If targetEntry's document is navigable's document, and targetEntry's document state's reload pending is
        //    false, then extend navigablesToCheck with the child navigables of navigable.
        if (target_entry->document() == navigable->active_document() && !target_entry->document_state()->reload_pending()) {
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-only-need-history-object-length/index-update
Vector<GC::Root<Navigable>> TraversableNavigable::get_all_navigables_that_only_need_history_object_length_index_update(int target_step) const
{
    // NOTE: Other navigables might not be impacted by the traversal. For example, if the response is a 204, the currently active document will remain.
    //       Additionally, going 'back' after a 204 will change the current session history entry, but the active session history entry will already be correct.

    // 1. Let results be an empty list.
    Vector<GC::Root<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<GC::Root<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry is navigable's current session history entry and targetEntry's document state's reload pending is false, then:
        if (target_entry == navigable->current_session_history_entry() && !target_entry->document_state()->reload_pending()) {
            // 1.  Append navigable to results.
            results.append(navigable);

            // 2. Extend navigablesToCheck with navigable's child navigables.
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-navigables-that-might-experience-a-cross-document-traversal
Vector<GC::Root<Navigable>> TraversableNavigable::get_all_navigables_that_might_experience_a_cross_document_traversal(int target_step) const
{
    // NOTE: From traversable's session history traversal queue's perspective, these documents are candidates for going cross-document during the
    //       traversal described by targetStep. They will not experience a cross-document traversal if the status code for their target document is
    //       HTTP 204 No Content.
    //       Note that if a given navigable might experience a cross-document traversal, this algorithm will return navigable but not its child navigables.
    //       Those would end up unloaded, not traversed.

    // 1. Let results be an empty list.
    Vector<GC::Root<Navigable>> results;

    // 2. Let navigablesToCheck be « traversable ».
    Vector<GC::Root<Navigable>> navigables_to_check;
    navigables_to_check.append(const_cast<TraversableNavigable&>(*this));

    // 3. For each navigable of navigablesToCheck:
    while (!navigables_to_check.is_empty()) {
        auto navigable = navigables_to_check.take_first();

        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(target_step);

        // 2. If targetEntry's document is not navigable's document or targetEntry's document state's reload pending is true, then append navigable to results.
        // NOTE: Although navigable's active history entry can change synchronously, the new entry will always have the same Document,
        //       so accessing navigable's document is reliable.
        if (target_entry->document() != navigable->active_document() || target_entry->document_state()->reload_pending()) {
            results.append(navigable);
        }

        // 3. Otherwise, extend navigablesToCheck with navigable's child navigables.
        //    Adding child navigables to navigablesToCheck means those navigables will also be checked by this loop.
        //    Child navigables are only checked if the navigable's active document will not change as part of this traversal.
        else {
            navigables_to_check.extend(navigable->child_navigables());
        }
    }

    // 4. Return results.
    return results;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#deactivate-a-document-for-a-cross-document-navigation
static void deactivate_a_document_for_cross_document_navigation(GC::Ref<DOM::Document> displayed_document, Optional<UserNavigationInvolvement>, GC::Ref<SessionHistoryEntry> target_entry, GC::Ref<GC::Function<void()>> after_potential_unloads)
{
    // 1. Let navigable be displayedDocument's node navigable.
    auto navigable = displayed_document->navigable();

    // 2. Let potentiallyTriggerViewTransition be false.
    auto potentially_trigger_view_transition = false;

    // FIXME: 3. Let isBrowserUINavigation be true if userNavigationInvolvement is "browser UI"; otherwise false.

    // FIXME: 4. Set potentiallyTriggerViewTransition to the result of calling can navigation trigger a cross-document
    //           view-transition? given displayedDocument, targetEntry's document, navigationType, and isBrowserUINavigation.

    // 5. If potentiallyTriggerViewTransition is false, then:
    if (!potentially_trigger_view_transition) {
        // FIXME: 1. Let firePageSwapBeforeUnload be the following step
        //            1. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and null.

        // 2. Set the ongoing navigation for navigable to null.
        navigable->set_ongoing_navigation({});

        // 3. Unload a document and its descendants given displayedDocument, targetEntry's document, afterPotentialUnloads, and firePageSwapBeforeUnload.
        displayed_document->unload_a_document_and_its_descendants(target_entry->document(), after_potential_unloads);
    }
    // FIXME: 6. Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    else {
        // FIXME: 1. Let proceedWithNavigationAfterViewTransitionCapture be the following step:
        //            1. Append the following session history traversal steps to navigable's traversable navigable:
        //               1. Set the ongoing navigation for navigable to null.
        //               2. Unload a document and its descendants given displayedDocument, targetEntry's document, and afterPotentialUnloads.

        // FIXME: 2. Let viewTransition be the result of setting up a cross-document view-transition given displayedDocument,
        //           targetEntry's document, navigationType, and proceedWithNavigationAfterViewTransitionCapture.

        // FIXME: 3. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and viewTransition.

        // FIXME: 4. If viewTransition is null, then run proceedWithNavigationAfterViewTransitionCapture.

        TODO();
    }
}

struct ChangingNavigableContinuationState : public JS::Cell {
    GC_CELL(ChangingNavigableContinuationState, JS::Cell);
    GC_DECLARE_ALLOCATOR(ChangingNavigableContinuationState);

    GC::Ptr<DOM::Document> displayed_document;
    GC::Ptr<SessionHistoryEntry> target_entry;
    GC::Ptr<Navigable> navigable;
    bool update_only = false;

    GC::Ptr<SessionHistoryEntry> populated_target_entry;
    bool populated_cloned_target_session_history_entry = false;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(displayed_document);
        visitor.visit(target_entry);
        visitor.visit(navigable);
        visitor.visit(populated_target_entry);
    }
};

GC_DEFINE_ALLOCATOR(ChangingNavigableContinuationState);

// AD-HOC: Counter state for Phase E (non-changing navigable updates), allocated on the GC heap
// so it survives across queued global tasks.
struct NonChangingUpdateState : public JS::Cell {
    GC_CELL(NonChangingUpdateState, JS::Cell);
    GC_DECLARE_ALLOCATOR(NonChangingUpdateState);

    size_t total_jobs { 0 };
    size_t completed_jobs { 0 };
};

GC_DEFINE_ALLOCATOR(NonChangingUpdateState);

Vector<GC::Ref<Navigable>> TraversableNavigable::resolve_navigable_ids(Vector<String> const& ids)
{
    HashTable<StringView> id_set;
    for (auto const& id : ids)
        id_set.set(id);

    Vector<GC::Ref<Navigable>> result;

    // Check if the traversable itself is in the list.
    if (id_set.contains(id()))
        result.append(*this);

    // Traverse all descendant navigables looking for matching IDs.
    Vector<GC::Ref<Navigable>> to_visit;
    for (auto& child : child_navigables())
        to_visit.append(*child);

    while (!to_visit.is_empty()) {
        auto navigable = to_visit.take_first();
        if (id_set.contains(navigable->id()))
            result.append(navigable);
        for (auto& child : navigable->child_navigables())
            to_visit.append(*child);
    }

    return result;
}

// Phase B: Check if unloading is canceled (spec steps 2-5 of "apply the history step").
void TraversableNavigable::traversal_check_if_unloading_is_canceled(
    int step,
    GC::Ptr<SourceSnapshotParams> source_snapshot_params,
    GC::Ptr<Navigable> initiator_to_check,
    UserNavigationInvolvement user_involvement,
    GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> on_complete)
{
    // NOTE: The UI process pre-computes the used step and passes it as `step`.

    // 3. If initiatorToCheck is not null, then:
    if (initiator_to_check != nullptr) {
        auto change_or_reload_navigables = get_all_navigables_whose_current_session_history_entry_will_change_or_reload(step);

        // 1. Assert: sourceSnapshotParams is not null.
        VERIFY(source_snapshot_params);

        // 2. For each navigable of get all navigables whose current session history entry will change or reload:
        //    if initiatorToCheck is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then return "initiator-disallowed".
        for (auto const& navigable : change_or_reload_navigables) {
            if (!initiator_to_check->allowed_by_sandboxing_to_navigate(*navigable, *source_snapshot_params)) {
                on_complete->function()(CheckIfUnloadingIsCanceledResult::CanceledByNavigate);
                return;
            }
        }
    }

    // 4. Let navigablesCrossingDocuments be the result of getting all navigables that might experience a cross-document traversal given traversable and targetStep.
    auto navigables_crossing_documents = get_all_navigables_that_might_experience_a_cross_document_traversal(step);

    // 5. If checkForCancelation is true, and the result of checking if unloading is canceled given navigablesCrossingDocuments, traversable, targetStep,
    //    and userInvolvement is not "continue", then return that result.
    auto result = check_if_unloading_is_canceled(navigables_crossing_documents, *this, step, user_involvement);
    on_complete->function()(result);
}

// Phase CD setup: Set current session history entry and ongoing navigation for all changing navigables (spec step 8).
void TraversableNavigable::traversal_setup_changing_navigables(int step, Vector<String> changing_navigable_ids)
{
    // Resolve changing navigable IDs to live navigable objects.
    auto changing_navigables = resolve_navigable_ids(changing_navigable_ids);

    // 8. For each navigable of changingNavigables:
    for (auto& navigable : changing_navigables) {
        // 1. Let targetEntry be the result of getting the target history entry given navigable and targetStep.
        auto target_entry = navigable->get_the_target_history_entry(step);

        // 2. Set navigable's current session history entry to targetEntry.
        navigable->set_current_session_history_entry(target_entry);

        // 3. Set navigable's ongoing navigation to "traversal".
        navigable->set_ongoing_navigation(Traversal::Tag);
    }
}

// Phase CD per-navigable: Populate document and activate entry for one navigable (spec steps 12+14 per-navigable).
void TraversableNavigable::traversal_process_navigable(
    String navigable_id,
    int step,
    GC::Ptr<SourceSnapshotParams> source_snapshot_params,
    IGNORE_USE_IN_ESCAPING_LAMBDA UserNavigationInvolvement user_involvement,
    IGNORE_USE_IN_ESCAPING_LAMBDA Optional<Bindings::NavigationType> navigation_type,
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t script_history_length,
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t script_history_index,
    GC::Ref<GC::Function<void()>> on_complete)
{
    auto& vm = this->vm();

    // Resolve navigable ID to live navigable object.
    auto navigables = resolve_navigable_ids({ navigable_id });
    if (navigables.is_empty()) {
        on_complete->function()();
        return;
    }
    auto navigable = navigables.first();

    // AD-HOC: If the navigable has been destroyed, or has no active window, skip it.
    if (navigable->has_been_destroyed() || !navigable->active_window()) {
        on_complete->function()();
        return;
    }

    // 12. Queue a global task on the navigation and traversal task source of navigable's active window to run the steps:
    queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), GC::create_function(heap(), [this, &vm, navigable, source_snapshot_params, user_involvement, navigation_type, step, script_history_length, script_history_index, on_complete] {
        // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
        if (navigable->has_been_destroyed()) {
            on_complete->function()();
            return;
        }

        // --- Population phase (step 12 per-navigable) ---

        // 1. Let displayedEntry be navigable's active session history entry.
        auto displayed_entry = navigable->active_session_history_entry();

        // 2. Let targetEntry be navigable's current session history entry.
        auto target_entry = navigable->current_session_history_entry();

        // 3. Let changingNavigableContinuation be a changing navigable continuation state with:
        auto continuation = vm.heap().allocate<ChangingNavigableContinuationState>();
        continuation->displayed_document = displayed_entry->document();
        continuation->target_entry = target_entry;
        continuation->navigable = navigable;
        continuation->update_only = false;
        continuation->populated_target_entry = nullptr;
        continuation->populated_cloned_target_session_history_entry = false;

        // 6. Let oldOrigin be targetEntry's document state's origin.
        auto old_origin = target_entry->document_state()->origin();

        // After document is populated, proceed to activation (step 14 per-navigable).
        auto after_document_populated = [this, old_origin, continuation, &vm, navigable, step, script_history_length, script_history_index, navigation_type, user_involvement, on_complete](bool populated_cloned_target_she, GC::Ref<SessionHistoryEntry> populated_target_entry) mutable {
            continuation->populated_target_entry = populated_target_entry;
            continuation->populated_cloned_target_session_history_entry = populated_cloned_target_she;

            // 1. If targetEntry's document is null, then set changingNavigableContinuation's update-only to true.
            if (!populated_target_entry->document()) {
                continuation->update_only = true;
            } else {
                // 2. If targetEntry's document's origin is not oldOrigin, then set targetEntry's classic history API state to StructuredSerializeForStorage(null).
                if (populated_target_entry->document()->origin() != old_origin) {
                    populated_target_entry->set_classic_history_api_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));
                }

                // 3. If all of the following are true:
                if (navigable->parent() == nullptr
                    && !(populated_target_entry->document()->browsing_context()->is_auxiliary() && populated_target_entry->document()->browsing_context()->opener_browsing_context() != nullptr)
                    && populated_target_entry->document_state()->origin() != old_origin) {
                    populated_target_entry->document_state()->set_navigable_target_name(String {});
                }
            }

            // --- Activation phase (step 14 per-navigable) ---

            auto displayed_document = continuation->displayed_document;

            // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed()) {
                on_complete->function()();
                return;
            }

            // 9. Let entriesForNavigationAPI be the result of getting session history entries for the navigation API given navigable and targetStep.
            auto entries_for_navigation_api = get_session_history_entries_for_the_navigation_api(*navigable, step);

            // 12. In both cases, let afterPotentialUnloads be the following steps:
            bool const update_only = continuation->update_only;
            GC::Ptr<SessionHistoryEntry> const target_entry = continuation->target_entry;
            bool const populated_cloned_target_session_history_entry = continuation->populated_cloned_target_session_history_entry;
            auto after_potential_unload = GC::create_function(this->heap(), [navigable, update_only, target_entry, populated_target_entry, populated_cloned_target_session_history_entry, displayed_document, on_complete, script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), &heap = this->heap(), navigation_type] {
                if (populated_cloned_target_session_history_entry) {
                    target_entry->set_document_state(populated_target_entry->document_state());
                    target_entry->set_url(populated_target_entry->url());
                    target_entry->set_classic_history_api_state(populated_target_entry->classic_history_api_state());
                }

                // 1. Let previousEntry be navigable's active session history entry.
                GC::Ptr<SessionHistoryEntry> const previous_entry = navigable->active_session_history_entry();

                // 2. If changingNavigableContinuation's update-only is false, then activate history entry targetEntry for navigable.
                if (!update_only)
                    navigable->activate_history_entry(*target_entry);

                // 3. Let updateDocument be an algorithm step which performs update document for history step application.
                auto update_document = [script_history_length, script_history_index, entries_for_navigation_api = move(entries_for_navigation_api), target_entry, update_only, navigation_type, previous_entry] {
                    target_entry->document()->update_for_history_step_application(*target_entry, update_only, script_history_length, script_history_index, navigation_type, entries_for_navigation_api, previous_entry);
                };

                // 4. If targetEntry's document is equal to displayedDocument, then perform updateDocument.
                if (target_entry->document().ptr() == displayed_document.ptr()) {
                    update_document();
                }
                // 5. Otherwise, queue a global task on the navigation and traversal task source given targetEntry's document's relevant global object to perform updateDocument
                else {
                    queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*target_entry->document()), GC::create_function(heap, move(update_document)));
                }

                on_complete->function()();
            });

            // 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument, then:
            if (update_only || populated_target_entry->document().ptr() == displayed_document.ptr()) {
                // 1. Set the ongoing navigation for navigable to null.
                navigable->set_ongoing_navigation({});

                // 2. Queue a global task on the navigation and traversal task source given navigable's active window to perform afterPotentialUnloads.
                VERIFY(navigable->active_window());
                queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), after_potential_unload);
            }
            // 11. Otherwise:
            else {
                // 1. Assert: navigationType is not null.
                VERIFY(navigation_type.has_value());

                // 2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and afterPotentialUnloads.
                deactivate_a_document_for_cross_document_navigation(*displayed_document, user_involvement, *populated_target_entry, after_potential_unload);
            }
        };

        // 4. If displayedEntry is targetEntry and targetEntry's document state's reload pending is false, then:
        if (displayed_entry == target_entry && !target_entry->document_state()->reload_pending()) {
            // 1. Set changingNavigableContinuation's update-only to true.
            continuation->update_only = true;

            // 2. Enqueue changingNavigableContinuation on changingNavigableContinuations.
            // 3. Abort these steps.
            // AD-HOC: Instead of enqueuing, we invoke after_document_populated directly and return,
            //         since we process one navigable at a time in the phase protocol.
            after_document_populated(false, *target_entry);
            return;
        }

        // 5. Switch on navigationType:
        if (navigation_type.has_value()) {
            switch (navigation_type.value()) {
            case Bindings::NavigationType::Reload:
                // Assert: targetEntry's document state's reload pending is true.
                VERIFY(target_entry->document_state()->reload_pending());
                break;
            case Bindings::NavigationType::Traverse:
                // Assert: targetEntry's document state's ever populated is true.
                VERIFY(target_entry->document_state()->ever_populated());
                break;
            case Bindings::NavigationType::Replace:
                // Assert: targetEntry's step is displayedEntry's step.
                // FIXME: Assert ever populated is false (not possible yet because we populate before finalize).
                VERIFY(target_entry->step() == displayed_entry->step());
                break;
            case Bindings::NavigationType::Push:
                // FIXME: Add ever populated check, and fix the bug where top level traversable's step is not updated when a child navigable navigates
                // - "push": Assert: targetEntry's step is displayedEntry's step + 1 and targetEntry's document state's ever populated is false.
                VERIFY(target_entry != displayed_entry);
                VERIFY(target_entry->step().get<int>() > displayed_entry->step().get<int>());
                break;
            }
        }

        // 7. If all of the following are true:
        //   * navigable is not traversable;
        //   * targetEntry is not navigable's current session history entry; and
        //   * oldOrigin is the same as navigable's current session history entry's document state's origin,
        // then:
        if (!navigable->is_traversable()
            && target_entry != navigable->current_session_history_entry()
            && old_origin == navigable->current_session_history_entry()->document_state()->origin()) {
            auto navigation = active_window()->navigation();
            navigation->fire_a_traverse_navigate_event(*target_entry, user_involvement);
        }

        // 8. If targetEntry's document is null, or targetEntry's document state's reload pending is true, then:
        if (!target_entry->document() || target_entry->document_state()->reload_pending()) {
            auto target_snapshot_params = navigable->snapshot_target_snapshot_params();

            GC::Ptr<SourceSnapshotParams> potentially_target_specific_source_snapshot_params = source_snapshot_params;
            if (!potentially_target_specific_source_snapshot_params)
                potentially_target_specific_source_snapshot_params = navigable->active_document()->snapshot_source_snapshot_params();

            target_entry->document_state()->set_reload_pending(false);
            auto allow_POST = target_entry->document_state()->reload_pending();

            auto populated_target_entry = target_entry->clone();

            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(this->heap(), [populated_target_entry, potentially_target_specific_source_snapshot_params, target_snapshot_params, this, allow_POST, navigable, after_document_populated = GC::create_function(this->heap(), move(after_document_populated)), user_involvement] {
                auto signal_to_continue_session_history_processing = Core::Promise<Empty>::construct();
                navigable->populate_session_history_entry_document(
                    populated_target_entry,
                    *potentially_target_specific_source_snapshot_params,
                    target_snapshot_params,
                    user_involvement,
                    signal_to_continue_session_history_processing,
                    {},
                    Navigable::NullOrError {},
                    ContentSecurityPolicy::Directives::Directive::NavigationType::Other,
                    allow_POST,
                    GC::create_function(this->heap(), [this, after_document_populated, populated_target_entry]() mutable {
                        VERIFY(active_window());
                        queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), GC::create_function(this->heap(), [after_document_populated, populated_target_entry]() mutable {
                            after_document_populated->function()(true, populated_target_entry);
                        }));
                    }));
            }));
        }
        // Otherwise, run afterDocumentPopulated immediately.
        else {
            after_document_populated(false, *target_entry);
        }
    }));
}

// Phase E: Update non-changing navigables (spec steps 15-19).
void TraversableNavigable::traversal_update_non_changing_navigables(
    int target_step,
    Vector<String> non_changing_navigable_ids,
    size_t script_history_length,
    size_t script_history_index,
    GC::Ref<GC::Function<void()>> on_complete)
{
    // 15. Resolve non-changing navigable IDs to live navigable objects.
    auto non_changing_navigables = resolve_navigable_ids(non_changing_navigable_ids);

    // If no non-changing navigables, skip to finalization.
    if (non_changing_navigables.is_empty()) {
        // 20. Set traversable's current session history step to step.
        m_current_session_history_step = target_step;
        on_complete->function()();
        return;
    }

    // Allocate counter state on the GC heap so it survives across queued global tasks.
    auto update_state = vm().heap().allocate<NonChangingUpdateState>();
    update_state->total_jobs = non_changing_navigables.size();
    update_state->completed_jobs = 0;

    auto finalize = [this, target_step, on_complete, update_state] {
        if (update_state->completed_jobs == update_state->total_jobs) {
            // 20. Set traversable's current session history step to step.
            m_current_session_history_step = target_step;
            on_complete->function()();
        }
    };

    // 18. For each navigable of nonchangingNavigablesThatStillNeedUpdates, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    for (auto& navigable : non_changing_navigables) {
        // AD-HOC: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
        if (navigable->has_been_destroyed() || !navigable->active_window()) {
            ++update_state->completed_jobs;
            finalize();
            continue;
        }

        queue_global_task(Task::Source::NavigationAndTraversal, *navigable->active_window(), GC::create_function(heap(), [navigable, script_history_length, script_history_index, update_state, finalize = GC::create_function(heap(), move(finalize))] {
            // AD-HOC: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
            if (navigable->has_been_destroyed() || !navigable->active_window()) {
                ++update_state->completed_jobs;
                finalize->function()();
                return;
            }

            // 1. Let document be navigable's active document.
            auto document = navigable->active_document();

            // 2. Set document's history object's index to scriptHistoryIndex.
            document->history()->m_index = script_history_index;

            // 3. Set document's history object's length to scriptHistoryLength.
            document->history()->m_length = script_history_length;

            // 4. Increment completedNonchangingJobs.
            ++update_state->completed_jobs;

            // 19/20. When all done, signal completion to the UI process.
            finalize->function()();
        }));
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
TraversableNavigable::CheckIfUnloadingIsCanceledResult TraversableNavigable::check_if_unloading_is_canceled(
    Vector<GC::Root<Navigable>> navigables_that_need_before_unload,
    GC::Ptr<TraversableNavigable> traversable,
    Optional<int> target_step,
    Optional<UserNavigationInvolvement> user_involvement_for_navigate_events)
{
    // 1. Let documentsToFireBeforeunload be the active document of each item in navigablesThatNeedBeforeUnload.
    Vector<GC::Root<DOM::Document>> documents_to_fire_beforeunload;
    for (auto& navigable : navigables_that_need_before_unload)
        documents_to_fire_beforeunload.append(navigable->active_document());

    // 2. Let unloadPromptShown be false.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto unload_prompt_shown = false;

    // 3. Let finalStatus be "continue".
    IGNORE_USE_IN_ESCAPING_LAMBDA auto final_status = CheckIfUnloadingIsCanceledResult::Continue;

    // 4. If traversable was given, then:
    if (traversable) {
        // 1. Assert: targetStep and userInvolvementForNavigateEvent were given.
        // NOTE: This assertion is enforced by the caller.

        // 2. Let targetEntry be the result of getting the target history entry given traversable and targetStep.
        auto target_entry = traversable->get_the_target_history_entry(target_step.value());

        // 3. If targetEntry is not traversable's current session history entry, and targetEntry's document state's origin is the same as
        //    traversable's current session history entry's document state's origin, then:
        if (target_entry != traversable->current_session_history_entry() && target_entry->document_state()->origin() != traversable->current_session_history_entry()->document_state()->origin()) {
            // 1. Let eventsFired be false.
            IGNORE_USE_IN_ESCAPING_LAMBDA auto events_fired = false;

            // 2. Let needsBeforeunload be true if navigablesThatNeedBeforeUnload contains traversable; otherwise false.
            auto it = navigables_that_need_before_unload.find_if([&traversable](auto const& navigable) {
                return navigable.ptr() == traversable.ptr();
            });
            auto needs_beforeunload = it != navigables_that_need_before_unload.end();

            // 3. If needsBeforeunload is true, then remove traversable's active document from documentsToFireBeforeunload.
            if (needs_beforeunload) {
                documents_to_fire_beforeunload.remove_first_matching([&](auto& document) {
                    return document.ptr() == traversable->active_document().ptr();
                });
            }

            // 4. Queue a global task on the navigation and traversal task source given traversable's active window to perform the following steps:
            VERIFY(traversable->active_window());
            queue_global_task(Task::Source::NavigationAndTraversal, *traversable->active_window(), GC::create_function(heap(), [needs_beforeunload, user_involvement_for_navigate_events, traversable, target_entry, &final_status, &unload_prompt_shown, &events_fired] {
                // 1. if needsBeforeunload is true, then:
                if (needs_beforeunload) {
                    // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given traversable's active document and false.
                    auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = traversable->active_document()->steps_to_fire_beforeunload(false);

                    // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
                    if (unload_prompt_shown_for_this_document)
                        unload_prompt_shown = true;

                    // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
                    if (unload_prompt_canceled_by_this_document)
                        final_status = CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload;
                }

                // 2. If finalStatus is "canceled-by-beforeunload", then abort these steps.
                if (final_status == CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload)
                    return;

                // 3. Let navigation be traversable's active window's navigation API.
                VERIFY(traversable->active_window());
                auto navigation = traversable->active_window()->navigation();

                // 4. Let navigateEventResult be the result of firing a traverse navigate event at navigation given targetEntry and userInvolvementForNavigateEvent.
                VERIFY(target_entry);
                auto navigate_event_result = navigation->fire_a_traverse_navigate_event(*target_entry, *user_involvement_for_navigate_events);

                // 5. If navigateEventResult is false, then set finalStatus to "canceled-by-navigate".
                if (!navigate_event_result)
                    final_status = CheckIfUnloadingIsCanceledResult::CanceledByNavigate;

                // 6. Set eventsFired to true.
                events_fired = true;
            }));

            // 5. Wait for eventsFired to be true.
            main_thread_event_loop().spin_processing_tasks_with_source_until(Task::Source::NavigationAndTraversal,
                GC::create_function(heap(), [&] { return events_fired; }));

            // 6. If finalStatus is not "continue", then return finalStatus.
            if (final_status != CheckIfUnloadingIsCanceledResult::Continue)
                return final_status;
        }
    }

    // 5. Let totalTasks be the size of documentsToFireBeforeunload.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto total_tasks = documents_to_fire_beforeunload.size();

    // 6. Let completedTasks be 0.
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t completed_tasks = 0;

    // 7. For each document of documentsToFireBeforeunload, queue a global task on the navigation and traversal task source given document's relevant global object to run the steps:
    for (auto& document : documents_to_fire_beforeunload) {
        // NOTE: We don't capture `document` by value here because it is a GC::Root and we want to avoid reference cycles.
        queue_global_task(Task::Source::NavigationAndTraversal, relevant_global_object(*document), GC::create_function(heap(), [document = document.ptr(), &final_status, &completed_tasks, &unload_prompt_shown] {
            // 1. Let (unloadPromptShownForThisDocument, unloadPromptCanceledByThisDocument) be the result of running the steps to fire beforeunload given document and unloadPromptShown.
            auto [unload_prompt_shown_for_this_document, unload_prompt_canceled_by_this_document] = document->steps_to_fire_beforeunload(unload_prompt_shown);

            // 2. If unloadPromptShownForThisDocument is true, then set unloadPromptShown to true.
            if (unload_prompt_shown_for_this_document)
                unload_prompt_shown = true;

            // 3. If unloadPromptCanceledByThisDocument is true, then set finalStatus to "canceled-by-beforeunload".
            if (unload_prompt_canceled_by_this_document)
                final_status = CheckIfUnloadingIsCanceledResult::CanceledByBeforeUnload;

            // 4. Increment completedTasks.
            completed_tasks++;
        }));
    }

    // 8. Wait for completedTasks to be totalTasks.
    main_thread_event_loop().spin_processing_tasks_with_source_until(Task::Source::NavigationAndTraversal,
        GC::create_function(heap(), [&] { return completed_tasks == total_tasks; }));

    // 9. Return finalStatus.
    return final_status;
}

TraversableNavigable::CheckIfUnloadingIsCanceledResult TraversableNavigable::check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload)
{
    return check_if_unloading_is_canceled(move(navigables_that_need_before_unload), {}, {}, {});
}

Vector<GC::Ref<SessionHistoryEntry>> TraversableNavigable::get_session_history_entries_for_the_navigation_api(GC::Ref<Navigable> navigable, int target_step)
{
    // 1. Let rawEntries be the result of getting session history entries for navigable.
    auto raw_entries = navigable->get_session_history_entries();

    if (raw_entries.is_empty())
        return {};

    // 2. Let entriesForNavigationAPI be a new empty list.
    Vector<GC::Ref<SessionHistoryEntry>> entries_for_navigation_api;

    // 3. Let startingIndex be the index of the session history entry in rawEntries who has the greatest step less than or equal to targetStep.
    // FIXME: Use min/max_element algorithm or some such here
    int starting_index = 0;
    auto max_step = 0;
    for (auto i = 0u; i < raw_entries.size(); ++i) {
        auto const& entry = raw_entries[i];
        if (entry->step().has<int>()) {
            auto step = entry->step().get<int>();
            if (step <= target_step && step > max_step) {
                starting_index = static_cast<int>(i);
            }
        }
    }

    // 4. Append rawEntries[startingIndex] to entriesForNavigationAPI.
    entries_for_navigation_api.append(raw_entries[starting_index]);

    // 5. Let startingOrigin be rawEntries[startingIndex]'s document state's origin.
    auto starting_origin = raw_entries[starting_index]->document_state()->origin();

    // 6. Let i be startingIndex − 1.
    auto i = starting_index - 1;

    // 7. While i > 0:
    while (i > 0) {
        auto& entry = raw_entries[static_cast<unsigned>(i)];
        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto entry_origin = entry->document_state()->origin();
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;

        // 2. Prepend rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.prepend(entry);

        // 3. Set i to i − 1.
        --i;
    }

    // 8. Set i to startingIndex + 1.
    i = starting_index + 1;

    // 9. While i < rawEntries's size:
    while (i < static_cast<int>(raw_entries.size())) {
        auto& entry = raw_entries[static_cast<unsigned>(i)];
        // 1. If rawEntries[i]'s document state's origin is not same origin with startingOrigin, then break.
        auto entry_origin = entry->document_state()->origin();
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;

        // 2. Append rawEntries[i] to entriesForNavigationAPI.
        entries_for_navigation_api.append(entry);

        // 3. Set i to i + 1.
        ++i;
    }

    // 10. Return entriesForNavigationAPI.
    return entries_for_navigation_api;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#clear-the-forward-session-history
void TraversableNavigable::clear_the_forward_session_history()
{
    // FIXME: 1. Assert: this is running within navigable's session history traversal queue.

    // 2. Let step be the navigable's current session history step.
    auto step = current_session_history_step();

    // 3. Let entryLists be the ordered set « navigable's session history entries ».
    Vector<Vector<GC::Ref<SessionHistoryEntry>>&> entry_lists;
    entry_lists.append(session_history_entries());

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto& entry_list = entry_lists.take_first();

        // 1. Remove every session history entry from entryList that has a step greater than step.
        entry_list.remove_all_matching([step](auto& entry) {
            return entry->step().template get<int>() > step;
        });

        // 2. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state()->nested_histories()) {
                entry_lists.append(nested_history.entries);
            }
        }
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void TraversableNavigable::traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document)
{
    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    GC::Ptr<SourceSnapshotParams> source_snapshot_params = nullptr;
    GC::Ptr<Navigable> initiator_to_check = nullptr;

    // 2. Let userInvolvement be "browser UI".
    UserNavigationInvolvement user_involvement = UserNavigationInvolvement::BrowserUI;

    // 1. If sourceDocument is given, then:
    if (source_document) {
        // 1. Set sourceSnapshotParams to the result of snapshotting source snapshot params given sourceDocument.
        source_snapshot_params = source_document->snapshot_source_snapshot_params();

        // 2. Set initiatorToCheck to sourceDocument's node navigable.
        initiator_to_check = source_document->navigable();

        // 3. Set userInvolvement to "none".
        user_involvement = UserNavigationInvolvement::None;
    }

    // 4. Append the following session history traversal steps to traversable:
    // AD-HOC: Instead of enqueuing a closure on the WebContent-side queue, we store the GC objects
    // that can't cross IPC and send a request to the UI process to enqueue a TraversalCommand.
    // The UI resolves the delta to an absolute target step at dequeue time and orchestrates execution.
    Optional<u64> source_snapshot_and_initiator_id;
    if (source_snapshot_params || initiator_to_check)
        source_snapshot_and_initiator_id = store_source_snapshot_and_initiator(source_snapshot_params, initiator_to_check);

    page().client().page_did_request_traversal_by_delta(delta, source_snapshot_and_initiator_id, user_involvement);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#close-a-top-level-traversable
void TraversableNavigable::close_top_level_traversable()
{
    // 1. If traversable's is closing is true, then return.
    if (is_closing())
        return;

    // 2. Definitely close traversable.
    definitely_close_top_level_traversable();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#definitely-close-a-top-level-traversable
void TraversableNavigable::definitely_close_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let toUnload be traversable's active document's inclusive descendant navigables.
    auto to_unload = active_document()->inclusive_descendant_navigables();

    // 2. If the result of checking if unloading is canceled for toUnload is not "continue", then return.
    if (check_if_unloading_is_canceled(to_unload) != CheckIfUnloadingIsCanceledResult::Continue)
        return;

    // 3. Append the following session history traversal steps to traversable:
    auto operation_id = store_session_history_operation(GC::create_function(heap(), [this] {
        auto signal = Core::Promise<Empty>::construct();
        // 1. Let afterAllUnloads be an algorithm step which destroys traversable.
        auto after_all_unloads = GC::create_function(heap(), [this] {
            destroy_top_level_traversable();
        });

        // 2. Unload a document and its descendants given traversable's active document, null, and afterAllUnloads.
        active_document()->unload_a_document_and_its_descendants({}, after_all_unloads);
        signal->resolve({});
        return signal;
    }));
    page().client().page_did_request_session_history_operation(operation_id);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void TraversableNavigable::destroy_top_level_traversable()
{
    VERIFY(is_top_level_traversable());

    // 1. Let browsingContext be traversable's active browsing context.
    auto browsing_context = active_browsing_context();

    // 2. For each historyEntry in traversable's session history entries [[ in what order? ]]:
    for (auto& history_entry : m_session_history_entries) {
        // 1. Let document be historyEntry's document.
        auto document = history_entry->document();

        // 2. If document is not null, then destroy a document and its descendants given document.
        if (document)
            document->destroy_a_document_and_its_descendants();
    }

    // 3. Remove browsingContext.
    if (!browsing_context) {
        dbgln("TraversableNavigable::destroy_top_level_traversable: No browsing context?");
    } else {
        browsing_context->remove();
    }

    // 4. Remove traversable from the user interface (e.g., close or hide its tab in a tabbed browser).
    page().client().page_did_close_top_level_traversable();

    // 5. Remove traversable from the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().remove(this);

    // FIXME: 6. Invoke WebDriver BiDi navigable destroyed with traversable.

    // FIXME: Figure out why we need to do this... we shouldn't be leaking Navigables for all time.
    //        However, without this, we can keep stale destroyed traversables around.
    set_has_been_destroyed();
    all_navigables().remove(*this);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
Optional<int> finalize_a_same_document_navigation(GC::Ref<TraversableNavigable> traversable, GC::Ref<Navigable> target_navigable, GC::Ref<SessionHistoryEntry> target_entry, GC::Ptr<SessionHistoryEntry> entry_to_replace)
{
    // NOTE: This is not in the spec but we should not navigate destroyed navigable.
    if (target_navigable->has_been_destroyed())
        return {};

    // FIXME: 1. Assert: this is running on traversable's session history traversal queue.

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    if (target_navigable->active_session_history_entry() != target_entry) {
        return {};
    }

    // 3. Let targetStep be null.
    Optional<int> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto& target_entries = target_navigable->get_session_history_entries();

    // 5. If entryToReplace is null, then:
    // FIXME: Checking containment of entryToReplace should not be needed.
    //        For more details see https://github.com/whatwg/html/issues/10232#issuecomment-2037543137
    if (!entry_to_replace || !target_entries.contains_slow(GC::Ref { *entry_to_replace })) {
        // 1. Clear the forward session history of traversable.
        traversable->clear_the_forward_session_history();

        // 2. Set targetStep to traversable's current session history step + 1.
        target_step = traversable->current_session_history_step() + 1;

        // 3. Set targetEntry's step to targetStep.
        target_entry->set_step(*target_step);

        // 4. Append targetEntry to targetEntries.
        target_entries.append(target_entry);
    } else {
        // 1. Replace entryToReplace with targetEntry in targetEntries.
        *(target_entries.find(*entry_to_replace)) = target_entry;

        // 2. Set targetEntry's step to entryToReplace's step.
        target_entry->set_step(entry_to_replace->step());

        // 3. Set targetStep to traversable's current session history step.
        target_step = traversable->current_session_history_step();
    }

    // 6. Apply the push/replace history step targetStep to traversable.
    // AD-HOC: The caller drives the phase protocol via IPC instead of applying inline.
    return target_step;
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void TraversableNavigable::set_system_visibility_state(VisibilityState visibility_state)
{
    if (m_system_visibility_state == visibility_state)
        return;
    m_system_visibility_state = visibility_state;

    // When a user agent determines that the system visibility state for
    // traversable navigable traversable has changed to newState, it must run the following steps:

    // 1. Let navigables be the inclusive descendant navigables of traversable's active document.
    auto navigables = active_document()->inclusive_descendant_navigables();

    // 2. For each navigable of navigables:
    for (auto& navigable : navigables) {
        // 1. Let document be navigable's active document.
        auto document = navigable->active_document();
        VERIFY(document);

        // 2. Queue a global task on the user interaction task source given document's relevant global object
        //    to update the visibility state of document with newState.
        queue_global_task(Task::Source::UserInteraction, relevant_global_object(*document), GC::create_function(heap(), [visibility_state, document] {
            document->update_the_visibility_state(visibility_state);
        }));
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#currently-focused-area-of-a-top-level-traversable
GC::Ptr<DOM::Node> TraversableNavigable::currently_focused_area()
{
    // 1. If traversable does not have system focus, then return null.
    if (!is_focused())
        return nullptr;

    // 2. Let candidate be traversable's active document.
    auto candidate = active_document();

    // 3. While candidate's focused area is a navigable container with a non-null content navigable:
    //    set candidate to the active document of that navigable container's content navigable.
    while (candidate->focused_area()
        && is<NavigableContainer>(candidate->focused_area().ptr())
        && as<NavigableContainer>(*candidate->focused_area()).content_navigable()) {
        candidate = as<NavigableContainer>(*candidate->focused_area()).content_navigable()->active_document();
    }

    // 4. If candidate's focused area is non-null, set candidate to candidate's focused area.
    if (candidate->focused_area()) {
        // NOTE: We return right away here instead of assigning to candidate,
        //       since that would require compromising type safety.
        return candidate->focused_area();
    }

    // 5. Return candidate.
    return candidate;
}

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
Geolocation::EmulatedPositionData const& TraversableNavigable::emulated_position_data() const
{
    VERIFY(is_top_level_traversable());
    return m_emulated_position_data;
}

// https://w3c.github.io/geolocation/#dfn-emulated-position-data
void TraversableNavigable::set_emulated_position_data(Geolocation::EmulatedPositionData data)
{
    VERIFY(is_top_level_traversable());
    m_emulated_position_data = data;
}

void TraversableNavigable::process_screenshot_requests()
{
    auto& client = page().client();
    while (!m_screenshot_tasks.is_empty()) {
        auto task = m_screenshot_tasks.dequeue();
        if (task.node_id.has_value()) {
            auto* dom_node = DOM::Node::from_unique_id(*task.node_id);
            if (dom_node)
                dom_node->document().update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            if (!dom_node || !dom_node->paintable_box()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto rect = page().enclosing_device_rect(dom_node->paintable_box()->absolute_border_box_rect());
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>());
            if (bitmap_or_error.is_error()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto bitmap = bitmap_or_error.release_value();
            auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
            PaintConfig paint_config { .canvas_fill_rect = rect.to_type<int>() };
            render_screenshot(painting_surface, paint_config, [bitmap, &client] {
                client.page_did_take_screenshot(bitmap->to_shareable_bitmap());
            });
        } else {
            active_document()->update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            auto scrollable_overflow_rect = active_document()->layout_node()->paintable_box()->scrollable_overflow_rect();
            auto rect = page().enclosing_device_rect(scrollable_overflow_rect.value());
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>());
            if (bitmap_or_error.is_error()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto bitmap = bitmap_or_error.release_value();
            auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
            PaintConfig paint_config { .paint_overlay = true, .canvas_fill_rect = rect.to_type<int>() };
            render_screenshot(painting_surface, paint_config, [bitmap, &client] {
                client.page_did_take_screenshot(bitmap->to_shareable_bitmap());
            });
        }
    }
}

}
