#include "plugin/ring_skimmer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/types.h>

#include "plugin/native_graphics_system.h"

namespace nr {

using rex::graphics::xenos::Endian;
using rex::graphics::xenos::GpuSwap;
using rex::memory::RingBuffer;

namespace {
// Reported for occlusion queries (EVENT_WRITE_ZPD): everything visible.
constexpr uint32_t kFakeSampleCount = 1000;
}  // namespace

RingSkimmer::RingSkimmer(NativeGraphicsSystem* system, rex::memory::Memory* memory,
                         bool log_packets)
    : system_(system),
      memory_(memory),
      log_packets_(log_packets),
      write_ptr_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

RingSkimmer::~RingSkimmer() { Stop(); }

bool RingSkimmer::Start(rex::system::KernelState* kernel_state) {
  running_ = true;
  thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerMain();
        return 0;
      }));
  thread_->set_name("GPU Commands (native)");
  thread_->Create();
  return true;
}

void RingSkimmer::Stop() {
  if (!thread_) {
    return;
  }
  running_ = false;
  write_ptr_event_->Set();
  thread_->Wait(0, 0, 0, nullptr);
  thread_.reset();
}

void RingSkimmer::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
  REXGPU_INFO("rexgpu-native: ring buffer at {:08X}, {} bytes", ptr, primary_buffer_size_);
}

void RingSkimmer::EnableReadPointerWriteBack(uint32_t ptr, uint32_t /*block_size_log2*/) {
  read_ptr_writeback_ptr_ = ptr;
}

void RingSkimmer::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_event_->Set();
}

void RingSkimmer::WorkerMain() {
  while (running_) {
    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      // Nothing to do: spin briefly, then sleep on the write pointer event.
      uint32_t loop_count = 0;
      do {
        if (loop_count > 500) {
          rex::thread::Wait(write_ptr_event_.get(), true, std::chrono::milliseconds(5));
        }
        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (running_ && (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      if (!running_) {
        break;
      }
    }
    if (!primary_buffer_ptr_ || !primary_buffer_size_) {
      read_ptr_index_ = write_ptr_index;
      continue;
    }
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);
    if (read_ptr_writeback_ptr_) {
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                            read_ptr_index_);
    }
  }
}

uint32_t RingSkimmer::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("rexgpu-native: bad packet in the primary ring buffer");
      break;
    }
  } while (reader.read_count() && running_);
  return write_index;
}

void RingSkimmer::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("rexgpu-native: bad packet in an indirect buffer");
      break;
    }
  } while (reader.read_count() && running_);
}

bool RingSkimmer::ExecutePacket(RingBuffer* reader) {
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  if (packet == 0) {
    return true;
  }
  switch (packet >> 30) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return true;
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      return false;
  }
}

bool RingSkimmer::ExecutePacketType0(RingBuffer* reader, uint32_t packet) {
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("rexgpu-native: type 0 packet overflow");
    return false;
  }
  uint32_t base_index = packet & 0x7FFF;
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  for (uint32_t m = 0; m < count; m++) {
    uint32_t value = reader->ReadAndSwap<uint32_t>();
    if (log_packets_) {
      REXGPU_DEBUG("rexgpu-native: register {:04X} = {:08X}", write_one_reg ? base_index : base_index + m,
                   value);
    }
    WriteRegister(write_one_reg ? base_index : base_index + m, value);
  }
  return true;
}

bool RingSkimmer::ExecutePacketType1(RingBuffer* reader, uint32_t packet) {
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  WriteRegister(reg_index_1, reader->ReadAndSwap<uint32_t>());
  WriteRegister(reg_index_2, reader->ReadAndSwap<uint32_t>());
  return true;
}

bool RingSkimmer::ExecutePacketType3(RingBuffer* reader, uint32_t packet) {
  using namespace rex::graphics::xenos;
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("rexgpu-native: type 3 packet overflow (opcode {:02X})", opcode);
    return false;
  }
  // Predicated packets run only when the bin select passes; predicated swaps
  // never run.
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }
  if (log_packets_) {
    RingBuffer peek = *reader;
    uint32_t data[6] = {};
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 6); i++) {
      data[i] = peek.ReadAndSwap<uint32_t>();
    }
    REXGPU_DEBUG("rexgpu-native: packet opcode {:02X} count {}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 opcode, count, data[0], data[1], data[2], data[3], data[4], data[5]);
  }

  switch (opcode) {
    case PM4_INTERRUPT: {
      uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
      for (int n = 0; n < 6; n++) {
        if (cpu_mask & (1 << n)) {
          system_->DispatchInterruptCallback(1, n);
        }
      }
      return true;
    }
    case PM4_XE_SWAP: {
      uint32_t magic = reader->ReadAndSwap<uint32_t>();
      uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
      uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
      uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
      reader->AdvanceRead((count - 4) * sizeof(uint32_t));
      if (magic != kSwapSignature) {
        REXGPU_WARN("rexgpu-native: swap packet with bad signature {:08X}", magic);
      }
      system_->OnSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
      system_->counter()++;
      swaps_++;
      return true;
    }
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD: {
      uint32_t list_ptr = reader->ReadAndSwap<uint32_t>();
      uint32_t list_length = reader->ReadAndSwap<uint32_t>() & 0xFFFFF;
      ExecuteIndirectBuffer(list_ptr, list_length);
      return true;
    }
    case PM4_WAIT_REG_MEM:
      return ExecuteWaitRegMem(reader);
    case PM4_REG_RMW: {
      uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
      uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
      uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
      uint32_t value = ReadRegister(rmw_info & 0x1FFF);
      value &= ((rmw_info >> 31) & 1) ? ReadRegister(and_mask & 0x1FFF) : and_mask;
      value |= ((rmw_info >> 30) & 1) ? ReadRegister(or_mask & 0x1FFF) : or_mask;
      WriteRegister(rmw_info & 0x1FFF, value);
      return true;
    }
    case PM4_REG_TO_MEM: {
      uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
      uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();
      WriteGuestDword(mem_addr, ReadRegister(reg_addr));
      return true;
    }
    case PM4_MEM_WRITE: {
      uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
      for (uint32_t i = 0; i < count - 1; i++) {
        WriteGuestDword(write_addr, reader->ReadAndSwap<uint32_t>());
        write_addr += 4;
      }
      return true;
    }
    case PM4_COND_WRITE:
      return ExecuteCondWrite(reader);
    case PM4_EVENT_WRITE: {
      uint32_t initiator = reader->ReadAndSwap<uint32_t>();
      WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
    }
    case PM4_EVENT_WRITE_SHD: {
      // The game's fences: write a value, or the vblank / swap counter.
      uint32_t initiator = reader->ReadAndSwap<uint32_t>();
      uint32_t address = reader->ReadAndSwap<uint32_t>();
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      uint32_t data = ((initiator >> 31) & 1) ? system_->counter().load() : value;
      WriteGuestDword(address, data);
      return true;
    }
    case PM4_EVENT_WRITE_EXT: {
      // Screen extents of the previous draws: report the full surface.
      uint32_t initiator = reader->ReadAndSwap<uint32_t>();
      uint32_t address = reader->ReadAndSwap<uint32_t>();
      WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      uint16_t extents[] = {
          0 >> 3, kTexture2DCubeMaxWidthHeight >> 3, 0 >> 3, kTexture2DCubeMaxWidthHeight >> 3, 0, 1,
      };
      rex::memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address & ~uint32_t(3)),
                                              extents, 6);
      return true;
    }
    case PM4_EVENT_WRITE_ZPD: {
      // Occlusion queries: report a fixed number of passed samples when the
      // library marks the end of a query.
      const uint32_t kQueryFinished = rex::byte_swap(uint32_t(0xFFFFFEED));
      uint32_t initiator = reader->ReadAndSwap<uint32_t>();
      WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      uint32_t counts_address = ReadRegister(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
      if (counts_address) {
        auto* counts =
            memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(counts_address);
        bool is_end = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished ||
                      counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
        std::memset(counts, 0, sizeof(xe_gpu_depth_sample_counts));
        if (is_end) {
          counts->ZPass_A = kFakeSampleCount;
          counts->Total_A = kFakeSampleCount;
        }
      }
      return true;
    }
    case PM4_SET_CONSTANT:
      return ExecuteSetConstant(reader, count);
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS: {
      uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
      uint32_t index = offset_type & 0xFFFF;
      for (uint32_t i = 0; i < count - 1; i++) {
        WriteRegister(index + i, reader->ReadAndSwap<uint32_t>());
      }
      return true;
    }
    case PM4_SET_BIN_MASK_LO:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader->ReadAndSwap<uint32_t>();
      return true;
    case PM4_SET_BIN_MASK_HI:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(reader->ReadAndSwap<uint32_t>()) << 32);
      return true;
    case PM4_SET_BIN_SELECT_LO:
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | reader->ReadAndSwap<uint32_t>();
      return true;
    case PM4_SET_BIN_SELECT_HI:
      bin_select_ =
          (bin_select_ & 0xFFFFFFFFull) | (uint64_t(reader->ReadAndSwap<uint32_t>()) << 32);
      return true;
    case PM4_SET_BIN_MASK: {
      uint64_t hi = reader->ReadAndSwap<uint32_t>();
      uint64_t lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (hi << 32) | lo;
      return true;
    }
    case PM4_SET_BIN_SELECT: {
      uint64_t hi = reader->ReadAndSwap<uint32_t>();
      uint64_t lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (hi << 32) | lo;
      return true;
    }
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2: {
      // Draws are skipped; the register writes they carry are kept for
      // completeness of the register image.
      if (opcode == PM4_DRAW_INDX) {
        reader->ReadAndSwap<uint32_t>();  // viz query condition
        count--;
      }
      uint32_t initiator = reader->ReadAndSwap<uint32_t>();
      WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
    }
    case PM4_ME_INIT:
    case PM4_NOP:
    case PM4_LOAD_ALU_CONSTANT:
    case PM4_IM_LOAD:
    case PM4_IM_LOAD_IMMEDIATE:
    case PM4_INVALIDATE_STATE:
    case PM4_VIZ_QUERY:
    case PM4_CONTEXT_UPDATE:
    case PM4_WAIT_FOR_IDLE:
    default:
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
  }
}

bool RingSkimmer::ExecuteWaitRegMem(RingBuffer* reader) {
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();
  bool is_memory = (wait_info & 0x10) != 0;
  bool matched = false;
  const auto wait_start = std::chrono::steady_clock::now();
  bool reported = false;
  do {
    if (!reported && std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(2)) {
      reported = true;
      REXGPU_WARN(
          "rexgpu-native: WAIT_REG_MEM stuck for 2 s: {} {:08X} (value {:08X}) mask {:08X} "
          "function {} ref {:08X}",
          is_memory ? "memory" : "register", poll_reg_addr,
          is_memory ? ReadGuestDword(poll_reg_addr) : ReadRegister(poll_reg_addr), mask,
          wait_info & 7, ref);
    }
    uint32_t value;
    if (is_memory) {
      value = ReadGuestDword(poll_reg_addr);
    } else {
      if (poll_reg_addr == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
        // Cache flush request: nothing to flush, report it done.
        // (Directly: WriteRegister marks a write to it as a new request.)
        const_cast<volatile uint32_t&>(
            system_->registers()[rex::graphics::XE_GPU_REG_COHER_STATUS_HOST]) = 0;
      }
      value = ReadRegister(poll_reg_addr);
    }
    switch (wait_info & 0x7) {
      case 0x0:
        matched = false;
        break;
      case 0x1:
        matched = (value & mask) < ref;
        break;
      case 0x2:
        matched = (value & mask) <= ref;
        break;
      case 0x3:
        matched = (value & mask) == ref;
        break;
      case 0x4:
        matched = (value & mask) != ref;
        break;
      case 0x5:
        matched = (value & mask) >= ref;
        break;
      case 0x6:
        matched = (value & mask) > ref;
        break;
      default:
        matched = true;
        break;
    }
    if (!matched) {
      if (wait >= 0x100) {
        rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
        rex::thread::SyncMemory();
      } else {
        rex::thread::MaybeYield();
      }
      if (!running_) {
        return false;
      }
    }
  } while (!matched);
  return true;
}

bool RingSkimmer::ExecuteCondWrite(RingBuffer* reader) {
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value = (wait_info & 0x10) ? ReadGuestDword(poll_reg_addr) : ReadRegister(poll_reg_addr);
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x1:
      matched = (value & mask) < ref;
      break;
    case 0x2:
      matched = (value & mask) <= ref;
      break;
    case 0x3:
      matched = (value & mask) == ref;
      break;
    case 0x4:
      matched = (value & mask) != ref;
      break;
    case 0x5:
      matched = (value & mask) >= ref;
      break;
    case 0x6:
      matched = (value & mask) > ref;
      break;
    case 0x7:
      matched = true;
      break;
    default:
      break;
  }
  if (matched) {
    if (wait_info & 0x100) {
      WriteGuestDword(write_reg_addr, write_data);
    } else {
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool RingSkimmer::ExecuteSetConstant(RingBuffer* reader, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t base;
  switch (type) {
    case 0:  // ALU constants
      base = 0x4000;
      break;
    case 1:  // fetch constants
      base = 0x4800;
      break;
    case 2:  // bool constants
      base = 0x4900;
      break;
    case 3:  // loop constants
      base = 0x4908;
      break;
    case 4:  // registers
      base = 0x2000;
      break;
    default:
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  for (uint32_t i = 0; i < count - 1; i++) {
    WriteRegister(base + index + i, reader->ReadAndSwap<uint32_t>());
  }
  return true;
}

void RingSkimmer::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= NativeGraphicsSystem::kRegisterCount) {
    return;
  }
  uint32_t* regs = system_->registers();
  const_cast<volatile uint32_t&>(regs[index]) = value;
  // Scratch register writeback, as the SDK's CommandProcessor::WriteRegister
  // does: the library's fences are type 0 writes to SCRATCH_REG0..7 that the
  // GPU mirrors to SCRATCH_ADDR, and the game's WAIT_REG_MEM / CPU waits poll
  // that memory (the first run in the game hung on exactly that).
  if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
      index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
    if ((1u << scratch_reg) & regs[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
      uint32_t mem_addr = regs[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + scratch_reg * 4;
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
    }
  } else if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
    // Flush request pending; WAIT_REG_MEM on it reports it done.
    const_cast<volatile uint32_t&>(regs[index]) |= UINT32_C(0x80000000);
  }
}

uint32_t RingSkimmer::ReadRegister(uint32_t index) {
  if (index < NativeGraphicsSystem::kRegisterCount) {
    return system_->registers()[index];
  }
  return 0;
}

void RingSkimmer::WriteGuestDword(uint32_t address_with_endian, uint32_t value) {
  auto endianness = static_cast<Endian>(address_with_endian & 0x3);
  uint32_t address = address_with_endian & ~uint32_t(0x3);
  rex::memory::store(memory_->TranslatePhysical(address), GpuSwap(value, endianness));
}

uint32_t RingSkimmer::ReadGuestDword(uint32_t address_with_endian) {
  auto endianness = static_cast<Endian>(address_with_endian & 0x3);
  uint32_t address = address_with_endian & ~uint32_t(0x3);
  uint32_t value = rex::memory::load<uint32_t>(memory_->TranslatePhysical(address));
  return GpuSwap(value, endianness);
}

}  // namespace nr
