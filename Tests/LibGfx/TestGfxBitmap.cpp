/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NumericLimits.h>
#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>

TEST_CASE(create_wrapper_accepts_a_sufficient_buffer)
{
    Array<u8, 16> buffer {};
    auto bitmap = MUST(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, 8, buffer.span()));
    EXPECT_EQ(bitmap->width(), 2);
    EXPECT_EQ(bitmap->height(), 2);
    EXPECT_EQ(bitmap->pitch(), 8u);

    Array<u8, 32> larger {};
    EXPECT(!Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, 8, larger.span()).is_error());
}

TEST_CASE(create_wrapper_rejects_a_buffer_too_small_for_the_geometry)
{
    Array<u8, 8> buffer {};
    EXPECT(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, 8, buffer.span()).is_error());
}

TEST_CASE(create_wrapper_rejects_a_pitch_below_the_minimum)
{
    Array<u8, 16> buffer {};
    EXPECT(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, 4, buffer.span()).is_error());
}

TEST_CASE(create_wrapper_rejects_a_pitch_that_overflows_the_required_size)
{
    Array<u8, 16> buffer {};
    EXPECT(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 2 }, NumericLimits<size_t>::max(), buffer.span()).is_error());
}

TEST_CASE(create_wrapper_rejects_an_empty_size)
{
    Array<u8, 16> buffer {};
    EXPECT(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 0, 2 }, 8, buffer.span()).is_error());
    EXPECT(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { 2, 0 }, 8, buffer.span()).is_error());
    EXPECT(Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { -2, 2 }, 8, buffer.span()).is_error());
}
