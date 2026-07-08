// hypercore structured logger.
//
// Design goals for a hypervisor init/daemon context:
//   - No printf soup: every record is a level + message + typed key/value
//     fields, so logs are greppable and machine-parseable.
//   - Two renderings from one call site: human-readable "logfmt" for a TTY,
//     and line-delimited JSON for capture. Selectable at runtime because the
//     same binary runs interactively (dashboard TTY) and headless (boot).
//   - Zero dependencies. This runs in a minimal initramfs where we cannot
//     assume spdlog/fmt are present. C++20 <format> covers our needs.
//   - Thread-safe: the daemon will log from a socket-accept thread and from
//     per-VM health-check timers. A single mutex around the write is plenty
//     for our volume; this is not a high-throughput logging path.
//
// This is intentionally small. If volume ever justifies it we can swap the
// sink for an async ring buffer without touching call sites.
#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hypercore::log {

enum class Level { Trace, Debug, Info, Warn, Error };

constexpr std::string_view level_name(Level l) {
  switch (l) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info:  return "info";
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
  }
  return "?";
}

enum class Format { Logfmt, Json };

// A single structured field. Values are pre-rendered to string at the call
// site via the field() helper so the logger core stays type-agnostic.
struct Field {
  std::string key;
  std::string value;
  bool quote;  // whether value needs quoting/escaping in output
};

namespace detail {
inline std::string to_value(std::string_view s) { return std::string(s); }
inline std::string to_value(const char* s) { return std::string(s); }
inline std::string to_value(const std::string& s) { return s; }
inline std::string to_value(bool b) { return b ? "true" : "false"; }
template <typename T>
std::string to_value(const T& v) { return std::format("{}", v); }

template <typename T>
constexpr bool needs_quote() {
  // Note: string literals arrive here as `char*`/`const char*` after
  // array-to-pointer decay, so both must be covered or literal values would
  // be emitted unquoted (invalid JSON for e.g. a version like 0.1.0).
  return std::is_same_v<T, std::string> ||
         std::is_same_v<T, std::string_view> ||
         std::is_same_v<T, char*> ||
         std::is_same_v<T, const char*>;
}
}  // namespace detail

template <typename T>
Field field(std::string key, const T& value) {
  return Field{std::move(key), detail::to_value(value),
               detail::needs_quote<std::decay_t<T>>()};
}

// The logger. One instance owned by the process (see default_logger()), but
// constructible standalone for tests.
class Logger {
 public:
  explicit Logger(Level min = Level::Info, Format fmt = Format::Logfmt,
                  std::FILE* sink = stderr)
      : min_(min), fmt_(fmt), sink_(sink) {}

  void set_level(Level l) { min_ = l; }
  void set_format(Format f) { fmt_ = f; }
  Level level() const { return min_; }

  void log(Level lvl, std::string_view msg,
           const std::vector<Field>& fields = {}) {
    if (lvl < min_) return;
    std::string line = (fmt_ == Format::Json) ? render_json(lvl, msg, fields)
                                              : render_logfmt(lvl, msg, fields);
    std::lock_guard<std::mutex> guard(mu_);
    std::fputs(line.c_str(), sink_);
    std::fputc('\n', sink_);
    std::fflush(sink_);
  }

 private:
  static std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
      switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:   out += c;
      }
    }
    return out;
  }

  std::string render_logfmt(Level lvl, std::string_view msg,
                            const std::vector<Field>& fields) {
    std::string out = std::format("level={} msg=\"{}\"", level_name(lvl),
                                  escape(msg));
    for (const auto& f : fields) {
      if (f.quote)
        out += std::format(" {}=\"{}\"", f.key, escape(f.value));
      else
        out += std::format(" {}={}", f.key, f.value);
    }
    return out;
  }

  std::string render_json(Level lvl, std::string_view msg,
                          const std::vector<Field>& fields) {
    std::string out = std::format("{{\"level\":\"{}\",\"msg\":\"{}\"",
                                  level_name(lvl), escape(msg));
    for (const auto& f : fields) {
      // JSON: always quote keys; quote string values, emit others bare.
      if (f.quote)
        out += std::format(",\"{}\":\"{}\"", f.key, escape(f.value));
      else
        out += std::format(",\"{}\":{}", f.key, f.value);
    }
    out += "}";
    return out;
  }

  Level min_;
  Format fmt_;
  std::FILE* sink_;
  std::mutex mu_;
};

// Process-wide default logger. Header-only singleton (Meyers) so both daemon
// and CLI can log without threading a Logger& through every call.
inline Logger& default_logger() {
  static Logger instance;
  return instance;
}

// Convenience free functions over the default logger.
inline void trace(std::string_view m, std::vector<Field> f = {}) {
  default_logger().log(Level::Trace, m, f);
}
inline void debug(std::string_view m, std::vector<Field> f = {}) {
  default_logger().log(Level::Debug, m, f);
}
inline void info(std::string_view m, std::vector<Field> f = {}) {
  default_logger().log(Level::Info, m, f);
}
inline void warn(std::string_view m, std::vector<Field> f = {}) {
  default_logger().log(Level::Warn, m, f);
}
inline void error(std::string_view m, std::vector<Field> f = {}) {
  default_logger().log(Level::Error, m, f);
}

}  // namespace hypercore::log
