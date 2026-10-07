#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace kk {

// HTTPS GET into memory, following GitHub's redirects to its download host.
// Adds to `bytes` as data arrives and the response's size to `total`, when
// given. Windows only for now (WinHTTP); false elsewhere.
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes = nullptr,
             std::atomic<uint64_t>* total = nullptr);

}  // namespace kk
