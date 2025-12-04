/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2024, doctortheemh <doctortheemh@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <LibGfx/ImageFormats/ImageDecoderStream.h>
#include <LibGfx/ImageFormats/WebPLoader.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>

#include <core/SkCanvas.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/mux.h>

namespace Gfx {

struct WebPLoadingContext {
    WebPLoadingContext(NonnullRefPtr<ImageDecoderStream> stream)
        : stream(move(stream))
    {
    }

    ~WebPLoadingContext()
    {
        if (demuxer) {
            WebPDemuxDelete(demuxer);
            demuxer = nullptr;
        }

        if (current_frame_decoder) {
            WebPIDelete(current_frame_decoder);
            current_frame_decoder = nullptr;
        }
    }

    enum class State {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
        BitmapDecoded,
    };

    State state { State::NotDecoded };
    NonnullRefPtr<ImageDecoderStream> stream;

    // Image properties
    IntSize size;
    bool has_alpha;
    bool has_animation;
    size_t loop_count;
    ByteBuffer icc_data;
    WebPDemuxer* demuxer { nullptr };
    ByteBuffer demuxer_buffer;

    RefPtr<Bitmap> current_frame_bitmap;
    WebPIDecoder* current_frame_decoder { nullptr };

    WebPMuxAnimDispose previous_frame_dipose_method { WEBP_MUX_DISPOSE_NONE };
    RefPtr<PaintingSurface> animation_surface;

    Vector<ImageFrameDescriptor> frame_descriptors;

    ErrorOr<void> populate_demuxer_with_more_data()
    {
        if (demuxer) {
            WebPDemuxDelete(demuxer);
            demuxer = nullptr;
        }

        for (;;) {
            if (stream->is_eof())
                return {};

            constexpr size_t BUFFER_INCREMENT = 4 * KiB;
            auto bytes = TRY(demuxer_buffer.get_bytes_for_writing(BUFFER_INCREMENT));
            auto read_bytes = TRY(stream->read_some(bytes));
            if (read_bytes.size() < bytes.size())
                TRY(demuxer_buffer.try_resize(demuxer_buffer.size() - (bytes.size() - read_bytes.size())));

            WebPData data { demuxer_buffer.data(), demuxer_buffer.size() };
            WebPDemuxState demux_state;
            demuxer = WebPDemuxPartial(&data, &demux_state);

            if (demux_state == WEBP_DEMUX_PARSE_ERROR)
                return Error::from_string_literal("Failed to parse WebP");

            if (demuxer && (demux_state == WEBP_DEMUX_PARSED_HEADER || demux_state == WEBP_DEMUX_DONE))
                return {};

            if (demux_state != WEBP_DEMUX_PARSING_HEADER)
                return Error::from_string_literal("Expected demuxer to be parsing header");

            if (demuxer) {
                WebPDemuxDelete(demuxer);
                demuxer = nullptr;
            }
        }
    }
};

WebPImageDecoderPlugin::WebPImageDecoderPlugin(NonnullOwnPtr<WebPLoadingContext> context)
    : m_context(move(context))
{
}

WebPImageDecoderPlugin::~WebPImageDecoderPlugin() = default;

IntSize WebPImageDecoderPlugin::size()
{
    return m_context->size;
}

static ErrorOr<void> decode_webp_header(WebPLoadingContext& context)
{
    if (context.state >= WebPLoadingContext::State::HeaderDecoded)
        return {};

    TRY(context.populate_demuxer_with_more_data());

    auto format_flags = WebPDemuxGetI(context.demuxer, WEBP_FF_FORMAT_FLAGS);
    auto width = WebPDemuxGetI(context.demuxer, WEBP_FF_CANVAS_WIDTH);
    auto height = WebPDemuxGetI(context.demuxer, WEBP_FF_CANVAS_HEIGHT);
    auto loop_count = WebPDemuxGetI(context.demuxer, WEBP_FF_LOOP_COUNT);

    // Image header now decoded, save some results for fast access in other parts of the plugin.
    context.size = IntSize { width, height };
    context.has_animation = (format_flags & ANIMATION_FLAG) != 0;
    context.has_alpha = (format_flags & ALPHA_FLAG) != 0;
    context.loop_count = loop_count;

    if (context.has_animation)
        context.animation_surface = PaintingSurface::create_with_size(nullptr, context.size, context.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888, AlphaType::Unpremultiplied);

    if (format_flags & ICCP_FLAG) {
        WebPChunkIterator icc_profile;

        for (;;) {
            auto found = WebPDemuxGetChunk(context.demuxer, "ICCP", 1, &icc_profile);
            if (!found) {
                WebPDemuxReleaseChunkIterator(&icc_profile);
                TRY(context.populate_demuxer_with_more_data());
            } else {
                break;
            }
        }

        context.icc_data = TRY(context.icc_data.copy(icc_profile.chunk.bytes, icc_profile.chunk.size));
        WebPDemuxReleaseChunkIterator(&icc_profile);
    }

    context.state = WebPLoadingContext::State::HeaderDecoded;
    return {};
}

static ErrorOr<NonnullRefPtr<Bitmap>> decode_webp_frame(WebPLoadingContext& context, WebPIterator& frame)
{
    if (!context.current_frame_decoder) {
        auto bitmap_format = context.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
        context.current_frame_bitmap = TRY(Bitmap::create(bitmap_format, Gfx::AlphaType::Unpremultiplied, { frame.width, frame.height }));
        context.current_frame_decoder = WebPINewRGB(MODE_BGRA, context.current_frame_bitmap->scanline_u8(0), context.current_frame_bitmap->size_in_bytes(), context.current_frame_bitmap->pitch());
        if (!context.current_frame_decoder)
            return Error::from_string_literal("Failed to allocate WebP decoder");
    }

    VERIFY(context.current_frame_decoder);
    auto status_code = WebPIUpdate(context.current_frame_decoder, frame.fragment.bytes, frame.fragment.size);
    if (status_code == VP8_STATUS_OK) {
        WebPIDelete(context.current_frame_decoder);
        context.current_frame_decoder = nullptr;
        return *context.current_frame_bitmap;
    }

    if (status_code == VP8_STATUS_SUSPENDED)
        return Error::from_errno(EAGAIN);

    return Error::from_string_literal("Failed to decode WebP frame");
}

static ErrorOr<void> decode_webp_image(WebPLoadingContext& context)
{
    VERIFY(context.state >= WebPLoadingContext::State::HeaderDecoded);

    ScopeGuard update_state = [&context] {
        context.state = context.frame_descriptors.is_empty() ? WebPLoadingContext::State::Error : WebPLoadingContext::State::BitmapDecoded;
    };

    for (;;) {
        WebPIterator frame;
        ArmedScopeGuard free_frame = [&frame] {
            WebPDemuxReleaseIterator(&frame);
        };

        // We have to do + 1 here because 0 actually gives the last frame.
        size_t frame_index = context.frame_descriptors.size() + 1;
        while (!WebPDemuxGetFrame(context.demuxer, frame_index, &frame)) {
            WebPDemuxReleaseIterator(&frame);
            TRY(context.populate_demuxer_with_more_data());
            if (context.stream->is_eof())
                return {};
        }

        if (frame.width <= 0 || frame.height <= 0) {
            if (frame.complete)
                return Error::from_string_literal("Failed to decode WebP: Encountered an empty frame");

            free_frame.disarm();
            WebPDemuxReleaseIterator(&frame);
            TRY(context.populate_demuxer_with_more_data());
            continue;
        }

        auto maybe_error = decode_webp_frame(context, frame);
        if (!maybe_error.is_error()) {
            auto bitmap = maybe_error.release_value();

            if (context.has_animation) {
                VERIFY(context.animation_surface);
                auto immutable_bitmap = ImmutableBitmap::create(bitmap);
                IntRect destination_rect(frame.x_offset, frame.y_offset, frame.width, frame.height);

                auto& canvas = context.animation_surface->canvas();

                if (context.previous_frame_dipose_method == WEBP_MUX_DISPOSE_BACKGROUND)
                    canvas.clear(SkColors::kTransparent);

                context.previous_frame_dipose_method = frame.dispose_method;

                SkPaint paint;
                if (frame.has_alpha && frame.blend_method == WEBP_MUX_BLEND)
                    paint.setBlendMode(SkBlendMode::kSrcOver);

                canvas.drawImageRect(immutable_bitmap->sk_image(), to_skia_rect(destination_rect), to_skia_sampling_options(ScalingMode::None), &paint);

                auto bitmap_format = frame.has_alpha ? BitmapFormat::BGRA8888 : BitmapFormat::BGRx8888;
                auto final_bitmap = TRY(Bitmap::create(bitmap_format, Gfx::AlphaType::Unpremultiplied, context.size));
                context.animation_surface->read_into_bitmap(final_bitmap);
                context.frame_descriptors.append(ImageFrameDescriptor { final_bitmap, frame.duration });
            } else {
                context.frame_descriptors.append(ImageFrameDescriptor { bitmap, frame.duration });
            }
            continue;
        }

        auto error = maybe_error.release_error();
        if (error.is_errno() && error.code() == EAGAIN) {
            // We have to change the order of operations to free the frame before destroying the demuxer.
            free_frame.disarm();
            WebPDemuxReleaseIterator(&frame);
            TRY(context.populate_demuxer_with_more_data());
            continue;
        }

        return error;
    }
}

bool WebPImageDecoderPlugin::sniff(NonnullRefPtr<ImageDecoderStream> stream)
{
    WebPLoadingContext context(move(stream));
    return !decode_webp_header(context).is_error();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> WebPImageDecoderPlugin::create(NonnullRefPtr<ImageDecoderStream> stream)
{
    auto context = TRY(try_make<WebPLoadingContext>(move(stream)));
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) WebPImageDecoderPlugin(move(context))));
    TRY(decode_webp_header(*plugin->m_context));
    TRY(decode_webp_image(*plugin->m_context));
    return plugin;
}

bool WebPImageDecoderPlugin::is_animated()
{
    return m_context->has_animation;
}

size_t WebPImageDecoderPlugin::loop_count()
{
    if (!is_animated())
        return 0;

    return m_context->loop_count;
}

size_t WebPImageDecoderPlugin::frame_count()
{
    if (!is_animated())
        return 1;

    return m_context->frame_descriptors.size();
}

size_t WebPImageDecoderPlugin::first_animated_frame_index()
{
    return 0;
}

ErrorOr<ImageFrameDescriptor> WebPImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index >= frame_count())
        return Error::from_string_literal("WebPImageDecoderPlugin: Invalid frame index");

    if (m_context->state == WebPLoadingContext::State::Error)
        return Error::from_string_literal("WebPImageDecoderPlugin: Decoding failed");

    if (m_context->state != WebPLoadingContext::State::BitmapDecoded)
        return Error::from_string_literal("WebPImageDecoderPlugin: Frames not ready yet");

    if (index >= m_context->frame_descriptors.size())
        return Error::from_string_literal("WebPImageDecoderPlugin: Invalid frame index");
    return m_context->frame_descriptors[index];
}

ErrorOr<Optional<ReadonlyBytes>> WebPImageDecoderPlugin::icc_data()
{
    if (m_context->state < WebPLoadingContext::State::HeaderDecoded)
        (void)frame(0);

    if (!m_context->icc_data.is_empty())
        return m_context->icc_data;
    return OptionalNone {};
}

}
