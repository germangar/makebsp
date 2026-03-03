#pragma once

namespace tbb {

class task_arena {
public:
    task_arena(int max_concurrency = -1) {}
    template <typename F>
    void execute(F&& f) { f(); }
};

} // namespace tbb
