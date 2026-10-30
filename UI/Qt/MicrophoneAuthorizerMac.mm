/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <UI/Qt/MicrophoneAuthorizerMac.h>

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

namespace Ladybird {

// The answer comes back on an arbitrary queue. The callbacks that take it stay on the main thread, so only this
// atomically counted holder crosses threads.
struct PendingRequests : public AtomicRefCounted<PendingRequests> {
    Vector<Function<void(WebView::MicrophoneAuthorization)>> callbacks;
};

// The system asks the user on behalf of the process responsible for the browser: the app, or the terminal that ran it.
// The AudioServer that opens the device answers to the same one.
class MacMicrophoneAuthorizer final : public WebView::MicrophoneAuthorizer {
public:
    virtual WebView::MicrophoneAuthorization authorization() const override
    {
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusNotDetermined:
            return WebView::MicrophoneAuthorization::NotDetermined;
        case AVAuthorizationStatusAuthorized:
            return WebView::MicrophoneAuthorization::Authorized;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
            return WebView::MicrophoneAuthorization::Denied;
        }
        return WebView::MicrophoneAuthorization::Denied;
    }

    virtual void request_authorization(Function<void(WebView::MicrophoneAuthorization)> on_complete) override
    {
        // Requests made while the system is asking share its answer.
        m_pending_requests->callbacks.append(move(on_complete));
        if (m_pending_requests->callbacks.size() > 1)
            return;

        RefPtr<PendingRequests> pending_requests = m_pending_requests;
        RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL granted) {
                                     auto strong_event_loop = event_loop->take();
                                     if (!strong_event_loop)
                                         return;
                                     auto authorization = granted ? WebView::MicrophoneAuthorization::Authorized : WebView::MicrophoneAuthorization::Denied;
                                     strong_event_loop->deferred_invoke([pending_requests, authorization] {
                                         auto callbacks = move(pending_requests->callbacks);
                                         for (auto& callback : callbacks)
                                             callback(authorization);
                                     });
                                 }];
    }

    virtual void open_system_settings() override
    {
        auto* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"];
        [[NSWorkspace sharedWorkspace] openURL:url];
    }

private:
    NonnullRefPtr<PendingRequests> m_pending_requests { adopt_ref(*new PendingRequests) };
};

NonnullOwnPtr<WebView::MicrophoneAuthorizer> create_platform_microphone_authorizer()
{
    return make<MacMicrophoneAuthorizer>();
}

}
