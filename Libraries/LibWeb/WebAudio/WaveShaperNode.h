/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/WaveShaperNode.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>

namespace Web::WebAudio {

using OverSampleType = Bindings::OverSampleType;
using WaveShaperOptions = Bindings::WaveShaperOptions;

// https://webaudio.github.io/web-audio-api/#WaveShaperNode
class WaveShaperNode final : public AudioNode {
    WEB_WRAPPABLE(WaveShaperNode, AudioNode);
    GC_DECLARE_ALLOCATOR(WaveShaperNode);

public:
    virtual ~WaveShaperNode() override;

    static WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> create(GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> create_for_constructor(GC::Ref<BaseAudioContext>, WaveShaperOptions const& = {});

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 1; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    WebIDL::ExceptionOr<void> set_curve(GC::Ptr<JS::Float32Array>);
    WebIDL::ExceptionOr<GC::Ptr<JS::Float32Array>> curve() const;

    void set_oversample(OverSampleType);
    OverSampleType oversample() const { return m_oversample; }

private:
    WaveShaperNode(GC::Ref<BaseAudioContext>, WaveShaperOptions const&);

    WebIDL::ExceptionOr<void> set_curve(Optional<Vector<float>> new_curve);
    void queue_parameters_update();

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
    RefPtr<Rendering::WaveShaperCurve> m_curve;

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve-set-slot
    bool m_curve_set { false };

    // https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
    OverSampleType m_oversample { OverSampleType::None };
};

}
