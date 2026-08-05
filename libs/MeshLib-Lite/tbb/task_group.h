#pragma once

namespace tbb {

class task_group_context {
    bool cancelled = false;
public:
    bool is_group_execution_cancelled() const { return cancelled; }
    bool cancel_group_execution() { cancelled = true; return true; }
};

class task_group {
public:
    template <typename F>
    void run(F&& f) { f(); }
    void wait() {}
};

} // namespace tbb
