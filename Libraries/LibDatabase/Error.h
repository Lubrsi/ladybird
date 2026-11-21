/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibDatabase/Export.h>

namespace Database {

// https://www.sqlite.org/rescode.html
enum class ResultCode : int {
    NoMemory = 7,
    Full = 13,
    TooBig = 18,
    ConstraintPrimaryKey = 1555,
    ConstraintUnique = 2067,
};

class DATABASE_API Error {
public:
    static Error from_result_code(int extended_result_code);

    template<size_t N>
    static Error from_string_literal(char const (&string_literal)[N])
    {
        return AK::Error::from_string_literal(string_literal);
    }

    Error(AK::Error&& error)
        : m_error(move(error))
    {
    }

    StringView string_literal() const { return m_error.string_literal(); }

    Optional<int> result_code() const { return m_result_code; }
    bool has_result_code(ResultCode result_code) const { return m_result_code == to_underlying(result_code); }

    AK::Error const& error() const { return m_error; }

private:
    AK::Error m_error;
    Optional<int> m_result_code;
};

}

template<>
struct AK::Formatter<Database::Error> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Database::Error const& error)
    {
        return Formatter<FormatString>::format(builder, "{}"sv, error.error());
    }
};
