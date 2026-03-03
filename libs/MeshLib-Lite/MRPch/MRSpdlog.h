#pragma once
#include "MRFmt.h"
#include <memory>
#include <string>

namespace spdlog {
    enum class level { trace, debug, info, warn, err, critical, off };
    
    struct logger {
        template<typename... Args> void trace(Args&&...) {}
        template<typename... Args> void debug(Args&&...) {}
        template<typename... Args> void info(Args&&...) {}
        template<typename... Args> void warn(Args&&...) {}
        template<typename... Args> void error(Args&&...) {}
        template<typename... Args> void critical(Args&&...) {}
        void set_level(level) {}
    };

    inline std::shared_ptr<logger> get(const std::string&) { return std::make_shared<logger>(); }
    inline std::shared_ptr<logger> default_logger() { return std::make_shared<logger>(); }
    
    template<typename... Args> void info(Args&&...) {}
    template<typename... Args> void warn(Args&&...) {}
    template<typename... Args> void error(Args&&...) {}
    template<typename... Args> void critical(Args&&...) {}

    namespace sinks {
        template<typename T> struct basic_file_sink_mt {};
        template<typename T> struct stdout_color_sink_mt {};
    }
}
