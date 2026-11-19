/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDatabase/Error.h>

#include <sqlite3.h>

namespace Database {

static_assert(to_underlying(ResultCode::Full) == SQLITE_FULL);
static_assert(to_underlying(ResultCode::TooBig) == SQLITE_TOOBIG);
static_assert(to_underlying(ResultCode::ConstraintPrimaryKey) == SQLITE_CONSTRAINT_PRIMARYKEY);
static_assert(to_underlying(ResultCode::ConstraintUnique) == SQLITE_CONSTRAINT_UNIQUE);

Error Error::from_result_code(int extended_result_code)
{
    char const* description = sqlite3_errstr(extended_result_code);
    Error error { AK::Error::from_string_view({ description, __builtin_strlen(description) }) };
    error.m_result_code = extended_result_code;
    return error;
}

}
