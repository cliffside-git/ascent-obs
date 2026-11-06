#ifndef ASCENTOBS_COMMAND_FRAMER_H_
#define ASCENTOBS_COMMAND_FRAMER_H_

#include <cstddef>
#include <string>
#include <vector>

// Splits the bytes read from the desktop into JSON commands. The desktop
// writes one command per line ("{...}\n"), but a pipe read can hold several
// commands or only part of one, so bytes are buffered until a '\n'.
//
// Unchanged from before framing: a read that is exactly one command (with or
// without a trailing '\n', compact or pretty-printed) is passed on as is.
// An unterminated tail is passed on when it is one balanced {...} (a sender
// that doesn't end commands with '\n'); otherwise it waits for more bytes.
// An object has no balanced prefix, so the first half of a split command is
// never passed on. Malformed JSON still fails in the server's parse, as before.
//
// Not thread safe: one framer per connection, fed from its reader thread.
class CommandFramer {
 public:
  static const size_t kMaxPendingBytes = 1024 * 1024;

  enum class Result {
    kOk,
    // the unterminated tail grew past kMaxPendingBytes and was dropped
    kOverflow,
    // the unterminated tail can't be the start of a command (not '{') and
    // was dropped
    kGarbage,
  };

  Result Feed(const char* data,
              size_t size,
              std::vector<std::string>* commands) {
    if (pending_.empty()) {
      std::string chunk(data, size);
      if (IsOneJsonObject(chunk)) {
        commands->push_back(chunk);
        return Result::kOk;
      }
    }

    pending_.append(data, size);

    size_t start = 0;
    for (size_t newline = pending_.find('\n', start);
         newline != std::string::npos;
         newline = pending_.find('\n', start)) {
      AddLine(pending_.substr(start, newline - start), commands);
      start = newline + 1;
    }
    pending_.erase(0, start);

    size_t first = pending_.find_first_not_of(kWhitespace);
    if (first == std::string::npos) {
      pending_.clear();
      return Result::kOk;
    }

    if (pending_[first] != '{') {
      pending_.clear();
      return Result::kGarbage;
    }

    if (IsOneJsonObject(pending_)) {
      commands->push_back(pending_);
      pending_.clear();
      return Result::kOk;
    }

    if (pending_.size() > kMaxPendingBytes) {
      pending_.clear();
      return Result::kOverflow;
    }

    return Result::kOk;
  }

  size_t pending_size() const { return pending_.size(); }

  // true when |text| is one balanced {...} (strings and escapes respected)
  // with only whitespace around it. Cheap pre-check, not a JSON validator.
  static bool IsOneJsonObject(const std::string& text) {
    size_t i = text.find_first_not_of(kWhitespace);
    if (i == std::string::npos || text[i] != '{') {
      return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; i < text.size(); i++) {
      char c = text[i];
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }

      if (c == '"') {
        in_string = true;
      } else if (c == '{' || c == '[') {
        depth++;
      } else if (c == '}' || c == ']') {
        depth--;
        if (depth == 0) {
          return text.find_first_not_of(kWhitespace, i + 1) ==
                 std::string::npos;
        }
      }
    }

    return false;
  }

 private:
  static constexpr const char* kWhitespace = " \t\r\n";

  static void AddLine(std::string line, std::vector<std::string>* commands) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.find_first_not_of(kWhitespace) == std::string::npos) {
      return;
    }

    commands->push_back(line);
  }

  std::string pending_;
};

#endif  // ASCENTOBS_COMMAND_FRAMER_H_
