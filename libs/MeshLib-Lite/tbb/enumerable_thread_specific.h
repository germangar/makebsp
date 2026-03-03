#pragma once
#include <utility>
#include <type_traits>

namespace tbb {

template <typename T>
class enumerable_thread_specific {
public:
    enumerable_thread_specific() : val_() {}
    
    // Constructor for factory function
    template <typename F, typename = std::enable_if_t<std::is_invocable_r_v<T, F>>>
    explicit enumerable_thread_specific(F&& init_func) : val_(init_func()) {}

    // Constructor for prototypical value or variadic args
    template <typename ...Args, typename = std::enable_if_t<std::is_constructible_v<T, Args...>>>
    enumerable_thread_specific(Args&&... args) : val_(std::forward<Args>(args)...) {}

    T& local() { return val_; }
    const T& local() const { return val_; }
    
    T* begin() { return &val_; }
    T* end() { return &val_ + 1; }
    const T* begin() const { return &val_; }
    const T* end() const { return &val_ + 1; }

    void clear() { val_ = T(); }

private:
    T val_;
};

} // namespace tbb
