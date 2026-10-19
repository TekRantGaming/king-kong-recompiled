// King Kong change (task 300, thirdparty/NOTES.md): the bookkeeping of the placed texture pool
// (d3d12-texture.cpp, DeviceDesc::placedTextureHeapSize). Pure logic, no D3D12 types, so the native
// renderer's unit tests can check it (backend/tests/d3d12_placed_ranges_tests.cpp).
//
// Fixed-size blocks (one ID3D12Heap each); first fit over each block's free ranges, neighbours merge
// on free. Not thread safe: the pool locks around it.
#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

namespace nvrhi::d3d12
{
    class PlacedRanges
    {
    public:
        struct Allocation
        {
            uint32_t block = ~0u;
            uint64_t offset = 0;
            uint64_t size = 0;
            bool valid() const { return block != ~0u; }
        };

        explicit PlacedRanges(uint64_t blockSize) : m_BlockSize(blockSize) { }

        uint64_t blockSize() const { return m_BlockSize; }

        // A range of `size` bytes at a multiple of `alignment` (a power of two) in an existing block.
        // False when no block has room: the caller adds a block and tries again.
        bool allocate(uint64_t size, uint64_t alignment, Allocation& out)
        {
            if (size == 0 || size > m_BlockSize || alignment == 0 || (alignment & (alignment - 1)) != 0)
                return false;
            for (uint32_t b = 0; b < uint32_t(m_Blocks.size()); ++b)
            {
                Block& block = m_Blocks[b];
                if (!block.live || block.freeBytes < size)
                    continue;
                for (auto it = block.free.begin(); it != block.free.end(); ++it)
                {
                    const uint64_t start = it->first;
                    const uint64_t end = start + it->second;
                    const uint64_t aligned = (start + alignment - 1) & ~(alignment - 1);
                    if (aligned + size > end)
                        continue;
                    block.free.erase(it);
                    if (aligned > start)
                        block.free.emplace(start, aligned - start);
                    if (aligned + size < end)
                        block.free.emplace(aligned + size, end - aligned - size);
                    block.freeBytes -= size;
                    ++block.allocations;
                    out = Allocation{ b, aligned, size };
                    return true;
                }
            }
            return false;
        }

        // A new empty block; its index (a dead block's slot is reused).
        uint32_t addBlock()
        {
            uint32_t b = 0;
            while (b < uint32_t(m_Blocks.size()) && m_Blocks[b].live)
                ++b;
            if (b == m_Blocks.size())
                m_Blocks.emplace_back();
            Block& block = m_Blocks[b];
            block = Block{};
            block.live = true;
            block.freeBytes = m_BlockSize;
            block.free.emplace(0, m_BlockSize);
            return b;
        }

        // Returns the range. True when its block is now empty.
        bool release(const Allocation& a)
        {
            if (!a.valid() || a.block >= m_Blocks.size() || !m_Blocks[a.block].live)
                return false;
            Block& block = m_Blocks[a.block];
            uint64_t start = a.offset;
            uint64_t size = a.size;
            auto next = block.free.lower_bound(start);
            if (next != block.free.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == start)
                {
                    start = prev->first;
                    size += prev->second;
                    block.free.erase(prev);
                }
            }
            if (next != block.free.end() && next->first == a.offset + a.size)
            {
                size += next->second;
                block.free.erase(next);
            }
            block.free.emplace(start, size);
            block.freeBytes += a.size;
            --block.allocations;
            return block.allocations == 0;
        }

        // Drops an empty block (its heap is released by the caller).
        void removeBlock(uint32_t b)
        {
            if (b < m_Blocks.size() && m_Blocks[b].allocations == 0)
                m_Blocks[b] = Block{};
        }

        uint32_t liveBlocks() const
        {
            uint32_t n = 0;
            for (const Block& block : m_Blocks)
                n += block.live ? 1 : 0;
            return n;
        }
        uint32_t emptyBlocks() const
        {
            uint32_t n = 0;
            for (const Block& block : m_Blocks)
                n += (block.live && block.allocations == 0) ? 1 : 0;
            return n;
        }
        uint64_t freeBytes(uint32_t b) const { return b < m_Blocks.size() ? m_Blocks[b].freeBytes : 0; }
        size_t freeRanges(uint32_t b) const { return b < m_Blocks.size() ? m_Blocks[b].free.size() : 0; }

    private:
        struct Block
        {
            bool live = false;
            uint32_t allocations = 0;
            uint64_t freeBytes = 0;
            std::map<uint64_t, uint64_t> free; // offset -> size
        };
        uint64_t m_BlockSize;
        std::vector<Block> m_Blocks;
    };
}
