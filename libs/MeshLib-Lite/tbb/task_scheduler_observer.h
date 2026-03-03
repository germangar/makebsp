#pragma once

namespace tbb {

class task_scheduler_observer {
public:
    task_scheduler_observer() {}
    virtual ~task_scheduler_observer() {}
    void observe(bool) {}
    virtual void on_scheduler_entry(bool) {}
    virtual void on_scheduler_exit(bool) {}
};

} // namespace tbb
