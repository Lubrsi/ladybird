/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebView/MicrophoneAuthorizer.h>

namespace Ladybird {

NonnullOwnPtr<WebView::MicrophoneAuthorizer> create_platform_microphone_authorizer();

}
