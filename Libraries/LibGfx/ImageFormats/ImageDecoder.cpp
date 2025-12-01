/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SeekableSharedMemoryStream.h>
#include <LibGfx/ImageFormats/AVIFLoader.h>
#include <LibGfx/ImageFormats/BMPLoader.h>
#include <LibGfx/ImageFormats/GIFLoader.h>
#include <LibGfx/ImageFormats/ICOLoader.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/JPEGLoader.h>
#include <LibGfx/ImageFormats/JPEGXLLoader.h>
#include <LibGfx/ImageFormats/PNGLoader.h>
#include <LibGfx/ImageFormats/TIFFLoader.h>
#include <LibGfx/ImageFormats/TinyVGLoader.h>
#include <LibGfx/ImageFormats/WebPLoader.h>

namespace Gfx {

static ErrorOr<OwnPtr<ImageDecoderPlugin>> probe_and_sniff_for_appropriate_plugin(NonnullRefPtr<Core::SeekableSharedMemoryStream> stream)
{
    // For formats that don't require all data to be available before being able to start decoding.
    struct ImagePluginStreamingInitializer {
        bool (*sniff)(NonnullRefPtr<Core::SeekableSharedMemoryStream>) = nullptr;
        ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> (*create)(NonnullRefPtr<Core::SeekableSharedMemoryStream>) = nullptr;
    };

    static constexpr ImagePluginStreamingInitializer s_streaming_initializers[] = {
        { BMPImageDecoderPlugin::sniff, BMPImageDecoderPlugin::create },
        { GIFImageDecoderPlugin::sniff, GIFImageDecoderPlugin::create },
        // { ICOImageDecoderPlugin::sniff, ICOImageDecoderPlugin::create },
        { JPEGImageDecoderPlugin::sniff, JPEGImageDecoderPlugin::create },
        { JPEGXLImageDecoderPlugin::sniff, JPEGXLImageDecoderPlugin::create },
        { PNGImageDecoderPlugin::sniff, PNGImageDecoderPlugin::create },
        { TIFFImageDecoderPlugin::sniff, TIFFImageDecoderPlugin::create },
        { TinyVGImageDecoderPlugin::sniff, TinyVGImageDecoderPlugin::create },
        { AVIFImageDecoderPlugin::sniff, AVIFImageDecoderPlugin::create }
    };

    for (auto& plugin : s_streaming_initializers) {
        auto sniff_result = plugin.sniff(stream);
        dbgln("sniffed, was valid? {}, now seeking", sniff_result);
        TRY(stream->seek(0, SeekMode::SetPosition));
        dbgln("should have seeked");
        if (!sniff_result)
            continue;

        dbgln("now creating");
        return TRY(plugin.create(move(stream)));
    }

    // For formats that must have all the data available upfront to be able to decode.
    // struct ImagePluginNonStreamingInitializer {
    //     bool (*sniff)(ReadonlyBytes) = nullptr;
    //     ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> (*create)(ReadonlyBytes) = nullptr;
    // };
    //
    // static constexpr ImagePluginNonStreamingInitializer s_non_streaming_initializers[] = {
    //     { WebPImageDecoderPlugin::sniff, WebPImageDecoderPlugin::create },
    // };
    //
    // TRY(stream->seek(0, SeekMode::SetPosition));
    // auto bytes = TRY(stream->read_until_eof());


    return OwnPtr<ImageDecoderPlugin> {};
}

ErrorOr<ColorSpace> ImageDecoder::color_space()
{
    auto maybe_cicp = TRY(m_plugin->cicp());
    if (maybe_cicp.has_value())
        return ColorSpace::from_cicp(*maybe_cicp);

    auto maybe_icc_data = TRY(icc_data());
    if (!maybe_icc_data.has_value())
        return ColorSpace {};
    return ColorSpace::load_from_icc_bytes(maybe_icc_data.value());
}

ErrorOr<RefPtr<ImageDecoder>> ImageDecoder::try_create_for_stream(NonnullRefPtr<Core::SeekableSharedMemoryStream> stream, [[maybe_unused]] Optional<ByteString> mime_type)
{
    if (auto plugin = TRY(probe_and_sniff_for_appropriate_plugin(stream)); plugin)
        return adopt_ref_if_nonnull(new (nothrow) ImageDecoder(plugin.release_nonnull()));

    return RefPtr<ImageDecoder> {};
}

ImageDecoder::ImageDecoder(NonnullOwnPtr<ImageDecoderPlugin> plugin)
    : m_plugin(move(plugin))
{
}

}
