#pragma once
#include <cstddef>

namespace tbb {

class global_control {
public:
    enum parameter { max_allowed_parallelism };
    global_control(parameter p, size_t value) {}
    static size_t active_value(parameter p) { return 1; }
};

} // namespace tbb
