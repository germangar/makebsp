#pragma once
#include <utility>

namespace MR
{

// Minimal shim for boost::signals2::signal
template<typename T>
struct Signal
{
    Signal() noexcept = default;
    Signal( const Signal& ) noexcept {}
    Signal( Signal&& ) noexcept = default;
    Signal& operator =( const Signal& ) noexcept { return *this; }
    Signal& operator =( Signal&& ) noexcept = default;

    template<typename F>
    void connect(F&&) {}
    
    template<typename... Args>
    void operator()(Args&&...) const {}
    
    void disconnect_all_slots() {}
};

} //namespace MR
