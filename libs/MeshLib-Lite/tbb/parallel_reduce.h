#pragma once
#include <utility>
#include "blocked_range.h"

namespace tbb {

template <typename Range, typename Body>
void parallel_reduce(const Range& range, Body& body) {
    body(range);
}

template <typename Range, typename Body, typename Join>
void parallel_reduce(const Range& range, Body& body, const Join& join) {
    body(range);
}

template <typename Range, typename Value, typename RealBody, typename Reduction>
Value parallel_reduce(const Range& range, const Value& identity, const RealBody& real_body, const Reduction& reduction) {
    return real_body(range, identity);
}

template <typename Range, typename Value, typename RealBody, typename Reduction>
Value parallel_deterministic_reduce(const Range& range, const Value& identity, const RealBody& real_body, const Reduction& reduction) {
    return real_body(range, identity);
}

template <typename Range, typename Body>
void parallel_deterministic_reduce(const Range& range, Body& body) {
    body(range);
}

struct split {};

} // namespace tbb
