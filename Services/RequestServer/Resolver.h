/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibDNS/Resolver.h>
#include <RequestServer/Forward.h>

namespace RequestServer {

struct DNSOverUDPSocketInfo {
    Core::SocketAddress server_address;
};

struct DNSOverTLSSocketInfo {
    Core::SocketAddress server_address;
    ByteString server_hostname;
};

struct DNSOverHTTPSInfo {
    URL::URL resolver_url;
};

struct DNSOverSystemResolverInfo {
};

struct DNSInfo {
    static DNSInfo& the();

    Variant<DNSOverUDPSocketInfo, DNSOverTLSSocketInfo, DNSOverHTTPSInfo, DNSOverSystemResolverInfo> info { DNSOverSystemResolverInfo {} };
    bool validate_dnssec_locally { false };

private:
    DNSInfo() = default;
};

struct Resolver
    : public RefCounted<Resolver>
    , public Weakable<Resolver> {
    static NonnullRefPtr<Resolver> default_resolver();

    DNS::Resolver dns;

private:
    explicit Resolver(DNS::Resolver::CreateTunnelFunction create_tunnel);
};

ByteString const& default_certificate_path();
void set_default_certificate_path(ByteString);

}
