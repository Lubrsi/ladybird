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

// Names one arming of a body channel's waiter, so a wake owed to an earlier arming is recognizable.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, WakeTicket);

// A request id is meaningful only for the epoch of the client that minted it.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, RequestId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, TransportEpoch);

}
