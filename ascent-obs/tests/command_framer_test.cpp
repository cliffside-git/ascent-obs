// Splitting pipe reads into JSON commands (CommandFramer).
// Built and run against a local OBS build by scripts/run_tests.ps1.
#include <windows.h>
#include <obs.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "command_framer.h"

namespace {
int failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (cond) {                                              \
      printf("  ok: %s\n", #cond);                           \
    } else {                                                 \
      printf("FAIL: %s (line %d)\n", #cond, __LINE__);       \
      failures++;                                            \
    }                                                        \
  } while (0)

using Commands = std::vector<std::string>;

// a read that is one command is passed on raw (with its whitespace)
std::string Trim(const std::string& text) {
  size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// the parse the server runs on each command
bool Parses(const std::string& text) {
  OBSDataAutoRelease parsed = obs_data_create_from_json(text.c_str());
  return parsed != nullptr;
}

CommandFramer::Result Feed(CommandFramer& framer, const std::string& data,
                           Commands* out) {
  return framer.Feed(data.data(), data.size(), out);
}

const char kA[] = R"({"cmd":1,"identifier":7})";
const char kB[] = R"({"cmd":2,"text":"a } \" { b"})";

void TestSingleCommand() {
  printf("one command per read is passed on as is\n");
  {
    CommandFramer framer;
    Commands out;
    CHECK(Feed(framer, std::string(kA) + "\n", &out) ==
          CommandFramer::Result::kOk);
    CHECK(out.size() == 1 && Parses(out[0]));
    CHECK(framer.pending_size() == 0);
  }
  {
    printf("without a trailing newline\n");
    CommandFramer framer;
    Commands out;
    Feed(framer, kA, &out);
    CHECK(out.size() == 1 && Trim(out[0]) == kA);
    CHECK(framer.pending_size() == 0);
  }
  {
    printf("pretty-printed over several lines\n");
    CommandFramer framer;
    Commands out;
    Feed(framer, "{\n  \"cmd\": 1,\n  \"identifier\": 7\n}\n", &out);
    CHECK(out.size() == 1 && Parses(out[0]));
  }
}

void TestTwoCommandsOneRead() {
  printf("two commands in one read are both passed on, in order\n");
  CommandFramer framer;
  Commands out;
  Feed(framer, std::string(kA) + "\n" + kB + "\n", &out);
  // split path: exact lines, no newline left on them
  CHECK(out.size() == 2 && out[0] == kA && out[1] == kB);
  CHECK(framer.pending_size() == 0);

  printf("the second without a trailing newline\n");
  Commands out2;
  Feed(framer, std::string(kB) + "\n" + kA, &out2);
  CHECK(out2.size() == 2 && Trim(out2[0]) == kB && Trim(out2[1]) == kA);
}

void TestSplitCommand() {
  printf("one command split across two reads\n");
  CommandFramer framer;
  Commands out;
  std::string a = std::string(kB) + "\n";
  Feed(framer, a.substr(0, 10), &out);
  CHECK(out.empty());
  CHECK(framer.pending_size() == 10);
  Feed(framer, a.substr(10), &out);
  CHECK(out.size() == 1 && out[0] == kB);
  CHECK(framer.pending_size() == 0);

  printf("split before the newline, newline in the next read\n");
  Commands out2;
  Feed(framer, kB, &out2);
  Feed(framer, std::string("\n") + kA + "\n", &out2);
  // kB is complete on its own, so it goes out with the first read
  CHECK(out2.size() == 2 && Trim(out2[0]) == kB && Trim(out2[1]) == kA);
}

void TestTrailingPartialKept() {
  printf("a complete command plus a partial: partial kept for the next read\n");
  CommandFramer framer;
  Commands out;
  std::string b = kB;
  Feed(framer, std::string(kA) + "\n" + b.substr(0, 12), &out);
  CHECK(out.size() == 1 && out[0] == kA);
  CHECK(framer.pending_size() == 12);
  Feed(framer, b.substr(12) + "\n", &out);
  CHECK(out.size() == 2 && out[1] == kB);
}

void TestCrLf() {
  printf("\\r\\n line ends\n");
  CommandFramer framer;
  Commands out;
  Feed(framer, std::string(kA) + "\r\n" + kB + "\r\n", &out);
  CHECK(out.size() == 2 && out[0] == kA && out[1] == kB);
}

void TestEmptyLines() {
  printf("empty and blank lines are skipped\n");
  CommandFramer framer;
  Commands out;
  Feed(framer, std::string("\n\r\n  \n") + kA + "\n\n" + kB + "\n", &out);
  CHECK(out.size() == 2 && out[0] == kA && out[1] == kB);
  Commands out2;
  CHECK(Feed(framer, "\n", &out2) == CommandFramer::Result::kOk);
  CHECK(out2.empty());
  CHECK(framer.pending_size() == 0);
}

void TestOversize() {
  printf("an unterminated tail over the cap is dropped\n");
  CommandFramer framer;
  Commands out;
  std::string chunk(8000, 'x');
  CHECK(Feed(framer, "{\"cmd\":1,\"pad\":\"", &out) ==
        CommandFramer::Result::kOk);
  CommandFramer::Result result = CommandFramer::Result::kOk;
  size_t fed = 0;
  while (result == CommandFramer::Result::kOk &&
         fed <= 2 * 1024 * 1024) {  // fixed bound: the cap must be ~1 MiB
    result = Feed(framer, chunk, &out);
    fed += chunk.size();
  }
  CHECK(result == CommandFramer::Result::kOverflow);
  CHECK(fed > 1024 * 1024 && fed <= 1024 * 1024 + chunk.size());
  CHECK(framer.pending_size() == 0);
  CHECK(out.empty());

  printf("the next command after an overflow still works\n");
  Feed(framer, std::string(kA) + "\n", &out);
  CHECK(out.size() == 1 && Trim(out[0]) == kA);
}

void TestGarbage() {
  printf("an unterminated tail that isn't '{' is dropped\n");
  CommandFramer framer;
  Commands out;
  CHECK(Feed(framer, "hello", &out) == CommandFramer::Result::kGarbage);
  CHECK(framer.pending_size() == 0);
  Feed(framer, std::string(kA) + "\n", &out);
  CHECK(out.size() == 1 && Trim(out[0]) == kA);
}

void TestIsOneJsonObject() {
  printf("IsOneJsonObject\n");
  CHECK(CommandFramer::IsOneJsonObject(kA));
  CHECK(CommandFramer::IsOneJsonObject(std::string(" ") + kB + " \r\n"));
  CHECK(!CommandFramer::IsOneJsonObject(std::string(kA) + kB));
  CHECK(!CommandFramer::IsOneJsonObject(std::string(kA) + "\n" + kB));
  CHECK(!CommandFramer::IsOneJsonObject(R"({"cmd":1)"));
  CHECK(!CommandFramer::IsOneJsonObject(R"({"t":"}")"));
  CHECK(CommandFramer::IsOneJsonObject(R"({"t":"\\"})"));
  CHECK(CommandFramer::IsOneJsonObject(R"({"a":[1,{"b":2}]})"));
  CHECK(!CommandFramer::IsOneJsonObject("[1]"));
  CHECK(!CommandFramer::IsOneJsonObject(""));
}
}  // namespace

int main(int, char**) {
  TestSingleCommand();
  TestTwoCommandsOneRead();
  TestSplitCommand();
  TestTrailingPartialKept();
  TestCrLf();
  TestEmptyLines();
  TestOversize();
  TestGarbage();
  TestIsOneJsonObject();

  printf(failures ? "FAILED (%d)\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
