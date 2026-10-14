// Real diagnostics implementation with only SDK configuration/log transport stubbed.
#include "diagnostics.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/logger.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>

static spdlog::sink_ptr test_sink;
namespace rex {
void AddSink(spdlog::sink_ptr sink) { test_sink = std::move(sink); }
namespace cvar {
std::string GetFlagByName(std::string_view key) {
  if (key == "kk_update_channel") return "beta";
  if (key == "native_backend") return "vulkan";
  if (key == "native_upscaler") return "off";
  if (key == "window_width") return "1280";
  return "";
}
}
}
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "supply private test output directory");
    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    kk::InitDiagnostics();
    for (const char* text : {"C:\\Users\\OtherPerson\\secret.txt", "C:/Users/OtherPerson/secret.txt",
         "\\\\server\\OtherPerson\\secret.txt", "/home/OtherPerson/secret.txt", "/Users/OtherPerson/secret.txt"}) {
      const auto clean = kk::RedactDiagnostics(std::string("error ") + text + "\nnext line");
      Check(clean.find("OtherPerson") == std::string::npos, "private path leaked");
      Check(clean.find("next line") != std::string::npos, "lost following line");
    }
    if (const char* user = std::getenv("USERNAME"))
      Check(kk::RedactDiagnostics(std::string("hello ") + user).find(user) == std::string::npos, "user leaked");
    spdlog::logger log("test", test_sink);
    log.warn("startup fallback to Classic: C:\\Users\\OtherPerson\\shader.pack");
    for (int i = 0; i < 20; ++i) log.error("test error {}: C:/Users/OtherPerson/log.txt", i);
    const auto report = kk::DiagnosticsReport();
    Check(report.find("Version: test043") != std::string::npos, "version missing");
    Check(report.find("kk_update_channel = beta") != std::string::npos, "channel missing");
    Check(report.find("Primary desktop mode (Windows") != std::string::npos, "desktop mode missing");
    Check(report.find("startup fallback to Classic") != std::string::npos, "fallback missing");
    Check(report.find("test error 19:") != std::string::npos, "latest error missing");
    Check(report.find("test error 0:") == std::string::npos, "unbounded errors");
    Check(report.find("OtherPerson") == std::string::npos, "report leaked a profile path");
    Check(report.size() < 32768, "report not bounded");
    std::ofstream(root / "diagnostics.txt", std::ios::binary) << report;
    const auto crash = root / "snapshot.txt";
    FILE* file = nullptr;
    fopen_s(&file, crash.string().c_str(), "wb");
    Check(file != nullptr, "snapshot file failed");
    kk::WriteCrashDiagnostics(file);
    std::fclose(file);
    std::ifstream in(crash, std::ios::binary);
    const std::string snapshot{std::istreambuf_iterator<char>(in), {}};
    Check(snapshot == report, "crash and clipboard report differ");
    std::thread producer([&] { for (int i = 0; i < 500; ++i) log.error("concurrent error {}", i); });
    for (int i = 0; i < 100; ++i) {
      FILE* concurrent = nullptr;
      fopen_s(&concurrent, (root / "concurrent.txt").string().c_str(), "wb");
      Check(concurrent != nullptr, "concurrent file failed");
      kk::WriteCrashDiagnostics(concurrent);
      std::fclose(concurrent);
    }
    producer.join();
    Check(!kk::HasCrashReports(root / "missing-root"), "missing root should not offer a crash");
    std::filesystem::create_directories(root / "crashes");
    std::ofstream(root / "crashes" / "hang-test.txt") << "hang";
    Check(!kk::HasCrashReports(root), "hang should not offer a crash");
    std::ofstream(root / "crashes" / "crash-test.txt") << "crash";
    Check(kk::HasCrashReports(root), "saved crash was not detected");
    std::cout << "PASS: privacy, bounded errors, fallback, shared snapshot, concurrent crash writes, crash discovery\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
