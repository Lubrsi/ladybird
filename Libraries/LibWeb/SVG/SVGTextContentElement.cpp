/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibWeb/Bindings/SVGTextContentElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGTextContentElement.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::SVG {

SVGTextContentElement::SVGTextContentElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGTextContentElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextContentElement);
    Base::initialize(realm);
}

Optional<TextAnchor> SVGTextContentElement::text_anchor() const
{
    if (!layout_node())
        return {};
    switch (layout_node()->computed_values().text_anchor()) {
    case CSS::TextAnchor::Start:
        return TextAnchor::Start;
    case CSS::TextAnchor::Middle:
        return TextAnchor::Middle;
    case CSS::TextAnchor::End:
        return TextAnchor::End;
    default:
        VERIFY_NOT_REACHED();
    }
}

Utf16String SVGTextContentElement::text_contents() const
{
    return child_text_content().trim_ascii_whitespace();
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getNumberOfChars
WebIDL::Long SVGTextContentElement::get_number_of_chars() const
{
    // FIXME: This should properly check if the element is rendered (display: none) and return 0 if so.
    //        It should also recursively count characters in child elements and normalize whitespace
    //        according to the white-space property.
    return static_cast<WebIDL::Long>(text_contents().length_in_code_units());
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getSubStringLength
WebIDL::ExceptionOr<float> SVGTextContentElement::get_sub_string_length(WebIDL::UnsignedLong charnum, WebIDL::UnsignedLong nchars) const
{
    auto text = text_contents();
    auto number_of_chars = text.length_in_code_units();

    // 1. Assign an index to each addressable character in the DOM within this element, where the first character has index 0.
    // 2. If charnum is greater than the highest index assigned to a character, then throw an IndexSizeError.
    //    NOTE: nchars being negative is not possible since it's unsigned long in the IDL.
    if (charnum >= number_of_chars)
        return WebIDL::IndexSizeError::create(realm(), "charnum is greater than the number of characters"_utf16);

    // Update layout to ensure we have the font information
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::SVGTextContentElementGetComputedTextLength);
    if (!layout_node())
        return 0;

    // 3. Let length be a length in user units, initialized to 0.
    // 4. For each addressable character with index in range [charnum, charnum + nchars):
    //    Add the advance of the typographic character to length.
    // 5. Return length.
    auto const& font = layout_node()->first_available_font();
    auto substring_length = min(static_cast<size_t>(nchars), number_of_chars - charnum);
    auto substring = text.utf16_view().substring_view(charnum, substring_length);
    return font.width(substring);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getComputedTextLength
float SVGTextContentElement::get_computed_text_length() const
{
    // 1. Let count be the value that would be returned if the getNumberOfChars method were called on this element.
    // 2. Let length be the value that would be returned if the getSubStringLength method were called
    //    on this element, passing 0 and count as arguments.
    // 3. Return length.
    // NOTE: We measure the full text directly, which is equivalent to getSubStringLength(0, getNumberOfChars()).
    auto text = text_contents();
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::SVGTextContentElementGetComputedTextLength);
    if (!layout_node())
        return 0;
    auto const& font = layout_node()->first_available_font();
    return font.width(text);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextContentElement__getExtentOfChar
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> SVGTextContentElement::get_extent_of_char(WebIDL::UnsignedLong charnum) const
{
    auto text = text_contents();
    auto number_of_chars = text.length_in_code_units();

    // 1. Let cluster be the result of finding the typographic character for the character at index charnum within the current element.
    // 2. If cluster is null, then throw an IndexSizeError.
    if (charnum >= number_of_chars)
        return WebIDL::IndexSizeError::create(realm(), "charnum is greater than the number of characters"_utf16);

    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::SVGTextContentElementGetComputedTextLength);
    if (!layout_node())
        return Geometry::DOMRect::create(realm());

    auto const& font = layout_node()->first_available_font();
    auto const& metrics = font.pixel_metrics();

    // 3. Let quad be the potentially rotated rectangle in the current element's coordinate system that is the glyph cell for cluster.
    // 4. Let rect be the rectangle that forms the tightest bounding box around quad in the current element's coordinate system.
    // FIXME: This does not account for rotation from text-on-a-path or the rotate attribute.

    // Calculate x position by measuring the advance of all characters before charnum
    float x = 0;
    if (charnum > 0) {
        auto prefix = text.utf16_view().substring_view(0, charnum);
        x = font.width(prefix);
    }

    // Calculate the width of the character at charnum
    // Handle surrogate pairs: if this is a high surrogate and the next is a low surrogate, measure both together
    size_t char_length = 1;
    if (charnum + 1 < number_of_chars) {
        auto code_unit = text.utf16_view().code_unit_at(charnum);
        auto next_code_unit = text.utf16_view().code_unit_at(charnum + 1);
        if (code_unit >= 0xD800 && code_unit <= 0xDBFF && next_code_unit >= 0xDC00 && next_code_unit <= 0xDFFF)
            char_length = 2;
    }
    auto char_view = text.utf16_view().substring_view(charnum, char_length);
    float width = font.width(char_view);

    // The glyph cell extends from baseline - ascent to baseline + descent
    // In SVG text coordinates, baseline is at y=0
    float y = -metrics.ascent;
    float height = metrics.ascent + metrics.descent;

    // 5. Return a newly created DOMRect object representing the rectangle rect.
    return Geometry::DOMRect::create(realm(), Gfx::FloatRect { x, y, width, height });
}

GC::Ref<Geometry::DOMPoint> SVGTextContentElement::get_start_position_of_char(WebIDL::UnsignedLong charnum)
{
    dbgln("(STUBBED) SVGTextContentElement::get_start_position_of_char(charnum={}). Called on: {}", charnum, debug_description());
    return Geometry::DOMPoint::from_point(vm(), Geometry::DOMPointInit {});
}

}
