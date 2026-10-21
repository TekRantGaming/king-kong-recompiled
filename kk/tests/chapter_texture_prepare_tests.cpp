#include "backend/chapter_texture_prepare.h"
#include <chrono>
#include <cstdio>

int main() {
  auto check = [](bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "failed: %s\n", name); std::exit(1); }
  };
  kknr::TextureFetch fetch;
  fetch.words[0] = 2 | (4u << 22);
  fetch.words[1] = 0x1000 | 6;
  fetch.words[2] = 127 | (127u << 13);
  fetch.words[3] = kknr::kSwizzleXYZW << 1;
  fetch.words[5] = 1u << 9;
  for (const uint32_t view : {0u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    auto header = fetch;
    header.words[1] |= view;
    header.words[5] |= view | 0x3000;
    const auto physical = nr::ChapterTextureFetch(header);
    check(physical.BaseAddress() == 0x1000 + (view == 0xE0000000 ? 0x1000 : 0), "base physical view offset");
    check(physical.MipAddress() == 0x3000 + (view == 0xE0000000 ? 0x1000 : 0), "mip physical view offset");
    check((physical.words[1] & 0xFFF) == (fetch.words[1] & 0xFFF), "address conversion preserves format");
  }
  auto relocated = fetch;
  relocated.words[1] += 0x20000;
  check(nr::ChapterTextureLayout(fetch) == nr::ChapterTextureLayout(relocated), "relocation preserves layout");
  auto different = fetch;
  different.words[1] ^= 1;
  check(!(nr::ChapterTextureLayout(fetch) == nr::ChapterTextureLayout(different)), "format separates layouts");
  different = fetch;
  different.words[2] ^= 1;
  check(!(nr::ChapterTextureLayout(fetch) == nr::ChapterTextureLayout(different)), "size separates layouts");
  nr::ChapterTextureInbox inbox;
  inbox.Push(0x12340000);
  auto messages = inbox.Take();
  check(messages.size() == 1 && messages[0] == 0x12340000, "guest object delivered");
  for (unsigned i = 0; i < 5000; ++i) inbox.Push(i + 1);
  check(inbox.Take().size() == 4096 && inbox.Take().empty(), "bounded inbox drains once");
  std::vector<uint8_t> live(0x50000);
  for (size_t i = 0; i < live.size(); ++i) live[i] = uint8_t(i * 17 + i / 257);
  kknr::GuestMemory memory{live.data(), live.size(), 0};
  nr::ChapterTexturePrepare worker;
  for (bool cancel : {false, true}) {
    std::vector<uint8_t> headers(65536);
    nr::StoreBE32(headers.data() + 0x120, 0x01030000);  // archive headers need not own a reference yet
    for (unsigned i = 0; i < 6; ++i) nr::StoreBE32(headers.data() + 0x130 + 4 * i, relocated.words[i]);
    auto job = std::make_unique<nr::ChapterTexturePrepare::Job>();
    job->scan = true;
    job->scan_begin = 0x40000000;
    job->scan_end = 0x40010000;
    job->layouts = {nr::ChapterTextureLayout(fetch)};
    job->copy = [&](uint32_t at, void* out, uint32_t n) {
      if (uint64_t(at) + n > 0x40010000 || at < 0x40000000) return false;
      std::memcpy(out, headers.data() + at - 0x40000000, n);
      return true;
    };
    job->cancelled = [cancel] { return cancel; };
    worker.Start(std::move(job));
    std::unique_ptr<nr::ChapterTexturePrepare::Job> done;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(done = worker.Take()) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(bool(done), "discovery completes");
    check(done->discovered.size() == (cancel ? 0u : 1u), "discovery obeys layout and cancellation");
    if (!cancel) check(done->discovered[0] == 0x40000120, "relocated live header discovered");
  }
  for (bool match : {true, false}) {
    auto job = std::make_unique<nr::ChapterTexturePrepare::Job>();
    job->fetch = relocated;
    const auto ranges = kknr::GetTextureRanges(relocated);
    const kknr::GuestRange source[2] = {{ranges.base, ranges.base_bytes}, {ranges.mip, ranges.mip_bytes}};
    check(job->snapshot.Capture(source, memory, job->source, nr::ChapterTexturePrepare::kMaxBytes), "snapshot capture");
    const uint64_t hash = job->snapshot.Hash();
    job->expected = {match ? hash : hash ^ 1};
    // Conversion must use the immutable snapshot, even when the live memory changes.
    std::fill(live.begin(), live.end(), 0);
    worker.Start(std::move(job));
    std::unique_ptr<nr::ChapterTexturePrepare::Job> done;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(done = worker.Take()) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(bool(done), "job finishes without caller joining");
    check(done->matched == match && done->converted == match, "only a full content match converts");
    check(done->hash == hash, "snapshot hash retained");
    if (match) check(done->data.bytes.size() == 65536 && done->data.bytes[1] != 0, "snapshot payload preserved");
  }
  std::puts("chapter texture preparation checks passed");
}
