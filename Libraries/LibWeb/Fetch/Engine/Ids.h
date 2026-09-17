/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Web::Fetch::Engine {

// Never reused within a process.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, FetchId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, DeliveryId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ContinuationId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, TransmissionId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, TransmissionGeneration);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, AttemptId);

// A request id is meaningful only for the epoch of the client that minted it.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, RequestId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, TransportEpoch);

// Advanced at every transfer, so a previous owner's request ending or end-of-body is recognizable as stale.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CustodyId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CustodyGeneration);

struct NavigationBodyCustody {
    CustodyId custody_id;
    CustodyGeneration custody_generation;

    bool operator==(NavigationBodyCustody const&) const = default;
};

}
