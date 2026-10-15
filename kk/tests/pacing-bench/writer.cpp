#include "benchmark_writer.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <windows.h>

using namespace kk::benchmark;
using namespace std::chrono_literals;
const std::string contents(4 * 1024 * 1024, 'k');
std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(file)), {}};
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--hard-exit") {
    ResultWriter writer;
    writer.Start([path = std::filesystem::path(argv[2])] {
      std::this_thread::sleep_for(100ms);
      if (!WriteResult(path, contents)) std::_Exit(2);
    });
    writer.Shutdown();  // The app's accepted-close hook precedes SDK _Exit.
    std::_Exit(0);
  }
  const auto root = std::filesystem::current_path() / ("benchmark-writer-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  const auto normal = root / "normal.txt";
  std::promise<void> entered, release;
  auto gate = release.get_future();
  {
    ResultWriter writer;
    writer.Start([&] {
      entered.set_value();
      gate.wait();
      WriteResult(normal, contents);
    });
    entered.get_future().wait();
    if (std::filesystem::exists(normal)) return 1;
    auto shutdown = std::async(std::launch::async, [&] { writer.Shutdown(); });
    const bool waited = shutdown.wait_for(50ms) == std::future_status::timeout;
    release.set_value();
    shutdown.get();
    if (!waited) return 2;
    if (Read(normal) != contents || writer.Start([] {})) return 3;
    writer.Shutdown();  // Repeated close/shutdown/destructor hooks are safe.
  }
  const auto destruct = root / "destructor.txt";
  { ResultWriter writer; writer.Start([&] { WriteResult(destruct, contents); }); }
  if (Read(destruct) != contents) return 4;
  // Saving is visible while Start still holds the registration lock. A close
  // at precisely that point must wait for the worker that is about to exist.
  const auto handoff = root / "handoff.txt";
  {
    ResultWriter writer;
    std::promise<void> starting, register_worker;
    auto registration = register_worker.get_future();
    auto start = std::async(std::launch::async, [&] {
      return writer.Start([&] { WriteResult(handoff, contents); }, [&] {
        starting.set_value();
        registration.wait();
      });
    });
    starting.get_future().wait();
    auto close = std::async(std::launch::async, [&] { writer.Shutdown(); });
    const bool waited = close.wait_for(50ms) == std::future_status::timeout;
    register_worker.set_value();
    const bool started = start.get();
    close.get();
    if (!waited || !started || Read(handoff) != contents) return 9;
  }
  if (WriteResult(root / "absent" / "results.txt", contents) ||
      std::filesystem::exists(root / "absent" / "results.txt")) return 5;
  // Rename failure must preserve the existing destination, never truncate it.
  const auto blocked = root / "blocked";
  std::filesystem::create_directory(blocked);
  std::ofstream(blocked / "keep.txt") << "keep";
  if (WriteResult(blocked, contents) || Read(blocked / "keep.txt") != "keep") return 6;
  const auto hard = root / "hard-exit.txt";
  std::string command = '"' + std::string(argv[0]) + "\" --hard-exit \"" + hard.string() + '"';
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &child)) return 7;
  const auto wait = WaitForSingleObject(child.hProcess, 10000);
  DWORD code = 1;
  GetExitCodeProcess(child.hProcess, &code);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  if (wait != WAIT_OBJECT_0 || code || Read(hard) != contents ||
      std::filesystem::exists(hard.string() + ".tmp")) return 8;
  std::cout << "Owned benchmark saves, IO failures and SDK-style hard exit: PASS\n";
}
