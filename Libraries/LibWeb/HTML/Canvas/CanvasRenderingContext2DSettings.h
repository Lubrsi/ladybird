/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ImageDataPrototype.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2dsettings
struct CanvasRenderingContext2DSettings {
    bool alpha { true };
    bool desynchronized { false };
    Bindings::PredefinedColorSpace color_space { Bindings::PredefinedColorSpace::Srgb };
    bool will_read_frequently { false };
};

JS::ThrowCompletionOr<CanvasRenderingContext2DSettings> convert_value_to_settings_dictionary(JS::VM&, JS::Value value);

}
