#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace raftdemo {

enum class LogLevel : std::uint8_t {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
};

inline std::mutex& GlobalLogMutex() {
  static std::mutex mu;
  return mu;
}

inline std::atomic<int>& GlobalLogLevelStorage() {
  static std::atomic<int> level{static_cast<int>(LogLevel::kInfo)};
  return level;
}

inline const char* LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
  }
  return "INFO";
}

inline bool TryParseLogLevel(const std::string& text, LogLevel* level) {
  if (level == nullptr) {
    return false;
  }
  if (text == "debug" || text == "DEBUG" || text == "Debug") {
    *level = LogLevel::kDebug;
    return true;
  }
  if (text == "info" || text == "INFO" || text == "Info") {
    *level = LogLevel::kInfo;
    return true;
  }
  if (text == "warn" || text == "WARN" || text == "warning" || text == "WARNING") {
    *level = LogLevel::kWarn;
    return true;
  }
  if (text == "error" || text == "ERROR" || text == "Error") {
    *level = LogLevel::kError;
    return true;
  }
  return false;
}

inline void SetGlobalLogLevel(LogLevel level) {
  GlobalLogLevelStorage().store(static_cast<int>(level));
}

inline LogLevel GetGlobalLogLevel() {
  return static_cast<LogLevel>(GlobalLogLevelStorage().load());
}

inline std::string NowString() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto tt = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif

  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
      << ms.count();
  return oss.str();
}

inline std::string EscapeLogMessage(const std::string& message) {
  std::string escaped;
  escaped.reserve(message.size());
  for (const char ch : message) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

template <typename... Args>
void LogWithLevel(LogLevel level, const std::string& tag, Args&&... args) {
  if (static_cast<int>(level) < static_cast<int>(GetGlobalLogLevel())) {
    return;
  }
  std::ostringstream oss;
  (oss << ... << std::forward<Args>(args));

  std::lock_guard<std::mutex> lk(GlobalLogMutex());
  std::cout << "ts=\"" << NowString() << "\" level=" << LogLevelName(level)
            << " tag=" << tag << " msg=\"" << EscapeLogMessage(oss.str()) << '"'
            << std::endl;
}

template <typename... Args>
void Log(const std::string& tag, Args&&... args) {
  LogWithLevel(LogLevel::kInfo, tag, std::forward<Args>(args)...);
}

template <typename... Args>
void LogDebug(const std::string& tag, Args&&... args) {
  LogWithLevel(LogLevel::kDebug, tag, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarn(const std::string& tag, Args&&... args) {
  LogWithLevel(LogLevel::kWarn, tag, std::forward<Args>(args)...);
}

template <typename... Args>
void LogError(const std::string& tag, Args&&... args) {
  LogWithLevel(LogLevel::kError, tag, std::forward<Args>(args)...);
}

}  // namespace raftdemo
