#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace kk {

// HTTPS GET into memory, following GitHub's redirects to its download host.
// Adds to `bytes` as data arrives and the response's size to `total`, when
// given. WinHTTP on Windows; curl elsewhere (false when it isn't installed).
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes = nullptr,
             std::atomic<uint64_t>* total = nullptr);

// HTTP(S) POST of a JSON body; the reply into `out`. True on a 200.
bool HttpPost(const std::string& url, const std::string& json, std::vector<uint8_t>& out);

}  // namespace kk
