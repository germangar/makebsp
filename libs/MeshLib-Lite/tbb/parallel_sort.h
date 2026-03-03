#pragma once
#include <algorithm>

namespace tbb {

template <typename RandomAccessIterator>
void parallel_sort(RandomAccessIterator begin, RandomAccessIterator end) {
    std::sort(begin, end);
}

template <typename RandomAccessIterator, typename Compare>
void parallel_sort(RandomAccessIterator begin, RandomAccessIterator end, const Compare& comp) {
    std::sort(begin, end, comp);
}

} // namespace tbb
