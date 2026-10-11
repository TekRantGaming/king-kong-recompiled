// The ring skimmer: a GPU command processor that executes nothing visible.
//
// The game's Direct3D library writes PM4 packets into its ring buffer and
// waits for the GPU to consume them: the read pointer write-back tells it how
// much ring space is free, EVENT_WRITE_SHD / MEM_WRITE packets write its fence
// values and the swap counter into memory, XE_SWAP (the runtime's VdSwap
// packet) is the frame flip, INTERRUPT raises the graphics interrupt. This
// thread walks the packets and does exactly those memory-side effects, so the
// library never blocks, and forwards the swap to the graphics system. Draws,
// constants, shader loads and everything else are skipped.
//
// Packet decoding follows the SDK's CommandProcessor (BSD, from Xenia).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/system/xobject.h>
#include <rex/thread.h>

namespace rex::memory {
class Memory;
class RingBuffer;
}  // namespace rex::memory
namespace rex::system {
class KernelState;
class XHostThread;
}  // namespace rex::system

namespace nr {

class NativeGraphicsSystem;

class RingSkimmer {
 public:
  RingSkimmer(NativeGraphicsSystem* system, rex::memory::Memory* memory, bool log_packets);
  ~RingSkimmer();

  bool Start(rex::system::KernelState* kernel_state);
  void Stop();

  // From the Vd* kernel exports (through the graphics system).
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);
  // From the CP_RB_WPTR register write. The ticket: the renderer's calls queued before the kick
  // (--native_render_thread; 0 without it).
  void UpdateWritePointer(uint32_t value, uint64_t ticket = 0);

 private:
  void WorkerMain();
  uint32_t ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index);
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count);
  bool ExecutePacket(rex::memory::RingBuffer* reader);
  bool ExecutePacketType0(rex::memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType1(rex::memory::RingBuffer* reader, uint32_t packet);
  bool ExecutePacketType3(rex::memory::RingBuffer* reader, uint32_t packet);
  bool ExecuteWaitRegMem(rex::memory::RingBuffer* reader);
  bool ExecuteCondWrite(rex::memory::RingBuffer* reader);
  bool ExecuteSetConstant(rex::memory::RingBuffer* reader, uint32_t count);
  void WriteRegister(uint32_t index, uint32_t value);
  // Before anything the CPU can see as the GPU's progress (a fence, a swap, an interrupt): the
  // render thread runs the calls queued before this batch's kick first, so a draw's guest data is
  // read before the game may reuse it. Once per batch.
  void WaitForRenderer();
  uint32_t ReadRegister(uint32_t index);
  void WriteGuestDword(uint32_t address_with_endian, uint32_t value);
  uint32_t ReadGuestDword(uint32_t address_with_endian);

  NativeGraphicsSystem* system_;
  rex::memory::Memory* memory_;
  bool log_packets_;

  std::atomic<bool> running_{false};
  // Statistics, logged every 10 seconds: WAIT_REG_MEM packets that had to wait, the time they
  // waited, and the time the worker spent executing packets.
  uint64_t waits_ = 0, wait_us_ = 0, busy_us_ = 0;
  std::chrono::steady_clock::time_point stats_logged_;
  rex::system::object_ref<rex::system::XHostThread> thread_;
  std::unique_ptr<rex::thread::Event> write_ptr_event_;
  std::atomic<uint32_t> write_ptr_index_{0};
  std::atomic<uint64_t> write_ticket_{0};
  uint64_t batch_ticket_ = 0;
  bool batch_waited_ = false;
  uint32_t read_ptr_index_ = 0;
  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;
  uint64_t bin_mask_ = 0xFFFFFFFFFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFFFFFFFFFull;
  uint64_t swaps_ = 0;
  // Type 3 packets by opcode (executed ones), logged with the statistics.
  uint64_t opcode_counts_[128] = {};
  uint64_t zpd_logged_ = 0;
};

}  // namespace nr
