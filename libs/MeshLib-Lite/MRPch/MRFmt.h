#pragma once
#include <string_view>
#include <string>

namespace fmt {
    template<typename... Args>
    inline std::string format(std::string_view f, Args&&...) { return std::string(f); }
    
    inline std::string_view runtime(std::string_view s) { return s; }

    template<typename T, typename Char = char>
    struct formatter {
        template<typename ParseContext>
        auto parse(ParseContext& ctx) { return ctx.begin(); }
        template<typename T2, typename FormatContext>
        auto format(const T2&, FormatContext& ctx) const { return ctx.out(); }
    };

    template<typename T>
    struct ptr {
        const T* p;
        ptr(const T* p) : p(p) {}
    };
}

namespace MR {
    inline std::string_view runtimeFmt( std::string_view str ) { return str; }
}
