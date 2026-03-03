#include "MRLog.h"
#include "MRPch/MRSpdlog.h"

namespace MR
{

Logger::Logger() : logger_( std::make_shared<spdlog::logger>() ) {}

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

const std::shared_ptr<spdlog::logger>& Logger::getSpdLogger() const
{
    return logger_;
}

std::string Logger::getDefaultPattern() const { return {}; }
void Logger::addSink( const spdlog::sink_ptr& ) {}
void Logger::removeSink( const spdlog::sink_ptr& ) {}
std::filesystem::path Logger::getLogFileName() const { return {}; }

} // namespace MR
