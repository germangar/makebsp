#pragma once

namespace tbb {

class task_group {
public:
    template <typename F>
    void run(F&& f) { f(); }
    void wait() {}
};

} // namespace tbb
