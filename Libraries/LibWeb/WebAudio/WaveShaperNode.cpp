/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAudio/AudioArray.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>
#include <LibWeb/WebAudio/WaveShaperNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(WaveShaperNode);

WaveShaperNode::WaveShaperNode(GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
    : AudioNode(context)
    , m_oversample(options.oversample)
{
}

WaveShaperNode::~WaveShaperNode() = default;

WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> WaveShaperNode::create(GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
{
    auto node = GC::Heap::the().allocate<WaveShaperNode>(context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#WaveShaperNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to maybe

    TRY(node->initialize_audio_node_options(options, default_options));

    // If options is given and specifies a curve, set [[curve set]] to true.
    if (options.curve.has_value())
        TRY(node->set_curve(*options.curve));

    node->queue_render_node_creation(make<Rendering::WaveShaperRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size()));
    node->queue_parameters_update();

    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-waveshapernode
WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> WaveShaperNode::create_for_constructor(GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
{
    return create(context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
WebIDL::ExceptionOr<void> WaveShaperNode::set_curve(GC::Ptr<JS::Float32Array> curve)
{
    // When this attribute is set, an internal copy of the curve is created by the WaveShaperNode. Subsequent
    // modifications of the contents of the array used to set the attribute therefore have no effect.
    Optional<Vector<float>> new_curve;
    if (curve)
        new_curve = copy_float32_array(*curve);

    TRY(set_curve(move(new_curve)));
    queue_parameters_update();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
WebIDL::ExceptionOr<void> WaveShaperNode::set_curve(Optional<Vector<float>> new_curve)
{
    // A InvalidStateError MUST be thrown if this attribute is set with a Float32Array that has a length less than 2.
    if (new_curve.has_value() && new_curve->size() < 2)
        return WebIDL::InvalidStateError::create("Curve must have at least 2 elements"_utf16);

    // 1. Let new curve be a Float32Array to be assigned to curve or null.
    // 2. If new curve is not null and [[curve set]] is true, throw an InvalidStateError and abort these steps.
    if (new_curve.has_value() && m_curve_set)
        return WebIDL::InvalidStateError::create("Curve has already been set"_utf16);

    // 3. If new curve is not null, set [[curve set]] to true.
    if (new_curve.has_value())
        m_curve_set = true;

    // 4. Assign new curve to the curve attribute.
    if (new_curve.has_value())
        m_curve = make_ref_counted<Rendering::WaveShaperCurve>(new_curve.release_value());
    else
        m_curve = nullptr;

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
WebIDL::ExceptionOr<GC::Ptr<JS::Float32Array>> WaveShaperNode::curve() const
{
    if (!m_curve)
        return nullptr;

    // NB: The internal copy is exposed instead of the array the attribute was set with, so the curve observed here
    //     always matches the one being rendered. This matches other engines.
    auto& realm = HTML::relevant_realm(relevant_global_object());
    auto array = TRY(JS::Float32Array::create(realm, m_curve->values.size()));
    overwrite_float32_array(*array, m_curve->values);
    return GC::Ptr<JS::Float32Array> { array };
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
void WaveShaperNode::set_oversample(OverSampleType oversample)
{
    m_oversample = oversample;
    queue_parameters_update();
}

void WaveShaperNode::queue_parameters_update()
{
    context()->queue_control_message(NodeMessage { SetWaveShaperParameters {
        .node_id = node_id(),
        .curve = m_curve,
        .oversample = m_oversample,
    } });
}

}
