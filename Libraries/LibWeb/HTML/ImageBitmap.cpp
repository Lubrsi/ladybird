/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/ImageBitmapPrototype.h>
#include <LibWeb/HTML/ImageBitmap.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ImageBitmap);

GC::Ref<ImageBitmap> ImageBitmap::create(JS::Realm& realm)
{
    return realm.create<ImageBitmap>(realm);
}

ImageBitmap::ImageBitmap(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void ImageBitmap::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ImageBitmap);
}

void ImageBitmap::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

WebIDL::ExceptionOr<void> ImageBitmap::serialization_steps(HTML::SerializationRecord&, bool, HTML::SerializationMemory&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::serialization_steps(HTML::SerializationRecord&, bool, HTML::SerializationMemory&)");
    return {};
}

WebIDL::ExceptionOr<void> ImageBitmap::deserialization_steps(ReadonlySpan<u32> const&, size_t&, HTML::DeserializationMemory&)
{
    // FIXME: Implement this
    dbgln("(STUBBED) ImageBitmap::deserialization_steps(ReadonlySpan<u32> const&, size_t&, HTML::DeserializationMemory&)");
    return {};
}

WebIDL::ExceptionOr<void> ImageBitmap::transfer_steps(HTML::TransferDataHolder& data_holder)
{
    // FIXME: 1. If value's origin-clean flag is not set, then throw a "DataCloneError" DOMException.

    // 2. Set dataHolder.[[BitmapData]] to value's bitmap data.
    data_holder.data.append(m_width);
    data_holder.data.append(m_height);
    data_holder.data.append(m_bitmap->pitch());
    data_holder.data.append(m_bitmap->pitch() >> 32);
    data_holder.data.append(to_underlying(m_bitmap->format()));
    data_holder.data.append(to_underlying(m_bitmap->alpha_type()));
    data_holder.data.append(m_bitmap->begin(), m_bitmap->data_size());

    // 3. Unset value's bitmap data.
    m_bitmap = nullptr;

    return {};
}

WebIDL::ExceptionOr<void> ImageBitmap::transfer_receiving_steps(HTML::TransferDataHolder&)
{
    // 1. Set value's bitmap data to dataHolder.[[BitmapData]].
    // m_width = data_holder.data.take_first();
    // m_height = data_holder.data.take_first();
    // size_t pitch = data_holder.data.take_first();
    // pitch |= static_cast<u64>(data_holder.data.take_first()) << 32;
    // auto format = static_cast<Gfx::BitmapFormat>(data_holder.data.take_first());
    // auto alpha_type = static_cast<Gfx::AlphaType>(data_holder.data.take_first());


    return {};
}

HTML::TransferType ImageBitmap::primary_interface() const
{
    return HTML::TransferType::ImageBitmap;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-width
WebIDL::UnsignedLong ImageBitmap::width() const
{
    // 1. If this's [[Detached]] internal slot's value is true, then return 0.
    if (is_detached())
        return 0;
    // 2. Return this's width, in CSS pixels.
    return m_width;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-height
WebIDL::UnsignedLong ImageBitmap::height() const
{
    // 1. If this's [[Detached]] internal slot's value is true, then return 0.
    if (is_detached())
        return 0;
    // 2. Return this's height, in CSS pixels.
    return m_height;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-close
void ImageBitmap::close()
{
    // 1. Set this's [[Detached]] internal slot value to true.
    set_detached(true);

    // 2. Unset this's bitmap data.
    m_bitmap = nullptr;
}

void ImageBitmap::set_bitmap(RefPtr<Gfx::Bitmap> bitmap)
{
    m_bitmap = move(bitmap);
    m_width = m_bitmap->width();
    m_height = m_bitmap->height();
}

Gfx::Bitmap* ImageBitmap::bitmap() const
{
    return m_bitmap.ptr();
}

}
