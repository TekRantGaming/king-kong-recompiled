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
  // From the CP_RB_WPTR register write.
  void UpdateWritePointer(uint32_t value);

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
  uint32_t ReadRegister(uint32_t index);
  void WriteGuestDword(uint32_t address_with_endian, uint32_t value);
  uint32_t ReadGuestDword(uint32_t address_with_endian);

  NativeGraphicsSystem* system_;
  rex::memory::Memory* memory_;
  bool log_packets_;

  std::atomic<bool> running_{false};
  rex::system::object_ref<rex::system::XHostThread> thread_;
  std::unique_ptr<rex::thread::Event> write_ptr_event_;
  std::atomic<uint32_t> write_ptr_index_{0};
  uint32_t read_ptr_index_ = 0;
  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;
  uint64_t bin_mask_ = 0xFFFFFFFFFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFFFFFFFFFull;
  uint64_t swaps_ = 0;
};

}  // namespace nr
