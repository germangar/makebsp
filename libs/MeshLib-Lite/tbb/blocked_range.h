#pragma once
#include <cstddef>

namespace tbb {

template <typename T>
class blocked_range {
public:
    using const_iterator = T;

    blocked_range(T begin, T end, size_t grainsize = 1) 
        : begin_(begin), end_(end), grainsize_(grainsize) {}

    T begin() const { return begin_; }
    T end() const { return end_; }
    size_t size() const { return static_cast<size_t>(end_ - begin_); }
    bool empty() const { return !(begin_ < end_); }
    size_t grainsize() const { return grainsize_; }

private:
    T begin_, end_;
    size_t grainsize_;
};

} // namespace tbb
