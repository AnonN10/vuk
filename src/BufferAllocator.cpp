#include "BufferAllocator.hpp"
#include "vuk/Allocator.hpp"
#include "vuk/Result.hpp"
#include "vuk/SourceLocation.hpp"

// Aligns given value down to nearest multiply of align value. For example: VmaAlignUp(11, 8) = 8.
// Use types like uint32_t, uint64_t as T.
template<typename T>
static inline T VmaAlignDown(T val, T align) {
	return val / align * align;
}

// Aligns given value up to nearest multiply of align value. For example: VmaAlignUp(11, 8) = 16.
// Use types like uint32_t, uint64_t as T.
template<typename T>
static inline T VmaAlignUp(T val, T align) {
	return (val + align - 1) / align * align;
}

namespace vuk {
	Result<void, AllocateException> BufferLinearAllocator::grow(size_t num_blocks, SourceLocationAtFrame source) {
		std::lock_guard _(mutex);

		int best_fit_block_size = 1024;
		int best_fit_index = -1;
		size_t actual_blocks = num_blocks;

		// find best fit allocation
		for (size_t i = 0; i < available_allocation_count; i++) {
			int block_over = (int)available_allocations[i].num_blocks - (int)num_blocks;
			if (block_over >= 0 && block_over < best_fit_block_size) {
				best_fit_block_size = block_over;
				best_fit_index = (int)i;
				if (block_over == 0) {
					break;
				}
			}
		}

		if (best_fit_index == -1) { // no allocation suitable, allocate new one
			Buffer alloc;
			BufferCreateInfo bci{ .mem_usage = mem_usage, .size = block_size * num_blocks };
			auto result = upstream->allocate_buffers(std::span{ &alloc, 1 }, std::span{ &bci, 1 }, source);
			if (!result) {
				return result;
			}
			for (auto i = 0; i < num_blocks; i++) {
				used_allocations[used_allocation_count + i] = { alloc, i > 0 ? 0 : num_blocks, 0 };
			}
			current_buffer += (int)num_blocks;
		} else { // we found one, we swap it into the used allocations and compact the available allocations
			std::swap(used_allocations[used_allocation_count], available_allocations[best_fit_index]);
			std::swap(available_allocations[best_fit_index], available_allocations[available_allocation_count - 1]);
			available_allocation_count--;

			auto& alloc = used_allocations[used_allocation_count];
			for (auto i = 1; i < alloc.num_blocks; i++) {
				// create 1 entry per block in used_allocations
				used_allocations[used_allocation_count + i] = { alloc.buffer, 0, 0 };
			}
			current_buffer += (int)used_allocations[used_allocation_count].num_blocks;
			actual_blocks = used_allocations[used_allocation_count].num_blocks;
		}
		used_allocations[0].base_address = 0;
		for (auto i = 0; i < actual_blocks; i++) {
			if (used_allocation_count + i == 0) {
				continue;
			}
			if (i == 0) { // the first block will have its based_address calculated, the remaining blocks share this address
				for (int j = (int)used_allocation_count - 1; j >= 0; j--) {
					if (used_allocations[j].num_blocks > 0) {
						used_allocations[used_allocation_count].base_address = used_allocations[j].base_address + used_allocations[j].num_blocks * block_size;
						break;
					}
				}
			} else {
				used_allocations[used_allocation_count + i].base_address = used_allocations[used_allocation_count + i - 1].base_address;
			}
		}
		used_allocation_count += actual_blocks;

		return {expected_value};
	}

	// lock-free bump allocation if there is still space
	Result<Buffer, AllocateException> BufferLinearAllocator::allocate_buffer(size_t size, size_t alignment, SourceLocationAtFrame source) {
		if (size == 0) {
			return { expected_value, Buffer{ .buffer = VK_NULL_HANDLE, .size = 0 } };
		}

		uint64_t old_needle = needle.load();
		uint64_t new_needle = VmaAlignUp(old_needle, alignment) + size;
		uint64_t low_buffer = old_needle / block_size;
		uint64_t high_buffer = new_needle / block_size;
		bool is_straddling = low_buffer != high_buffer;
		if (is_straddling) { // boost alignment to place on block start
			new_needle = VmaAlignUp(old_needle, block_size) + size;
			low_buffer = old_needle / block_size;
			high_buffer = new_needle / block_size;
			is_straddling = low_buffer != high_buffer;
		}
		while (!std::atomic_compare_exchange_strong(&needle, &old_needle, new_needle)) { // CAS loop
			old_needle = needle.load();
			new_needle = VmaAlignUp(old_needle, alignment) + size;
			low_buffer = old_needle / block_size;
			high_buffer = new_needle / block_size;
			is_straddling = low_buffer != high_buffer;
			if (is_straddling) { // boost alignment to place on block start
				new_needle = VmaAlignUp(old_needle, block_size) + size;
				low_buffer = old_needle / block_size;
				high_buffer = new_needle / block_size;
				is_straddling = low_buffer != high_buffer;
			}
		}

		uint64_t base = new_needle - size;
		int base_buffer = (int)(base / block_size);
		bool needs_to_create = old_needle == 0 || is_straddling;
		if (needs_to_create) {
			size_t num_blocks = std::max(high_buffer - low_buffer + (old_needle == 0 ? 1 : 0), static_cast<uint64_t>(1));
			while (current_buffer.load() < (int)high_buffer) {
				grow(num_blocks, source);
			}
			assert(base % block_size == 0);
		}
		// wait for the buffer to be allocated
		while (current_buffer.load() < (int)high_buffer) {
		};
		auto& current_alloc = used_allocations[base_buffer];
		auto offset = base - current_alloc.base_address;
		Buffer b = current_alloc.buffer;
		b.offset += offset;
		b.size = size;
		b.mapped_ptr = b.mapped_ptr != nullptr ? b.mapped_ptr + offset : nullptr;
		b.device_address = b.device_address != 0 ? b.device_address + offset : 0;

		return { expected_value, b };
	}

	void BufferLinearAllocator::reset() {
		std::lock_guard _(mutex);
		for (size_t i = 0; i < used_allocation_count;) {
			available_allocations[available_allocation_count++] = used_allocations[i];
			i += used_allocations[i].num_blocks;
		}
		used_allocations = {};
		used_allocation_count = 0;
		current_buffer = -1;
		needle = 0;
	}

	// we just destroy the buffers that we have left in the available allocations
	void BufferLinearAllocator::trim() {
		std::lock_guard _(mutex);
		for (size_t i = 0; i < available_allocation_count; i++) {
			auto& alloc = available_allocations[i];
			upstream->deallocate_buffers(std::span{ &alloc.buffer, 1 });
		}
		available_allocation_count = 0;
	}

	BufferLinearAllocator::~BufferLinearAllocator() {
		free();
	}

	void BufferLinearAllocator::free() {
		for (size_t i = 0; i < used_allocation_count; i++) {
			auto& buf = used_allocations[i].buffer;
			if (buf) {
				upstream->deallocate_buffers(std::span{ &buf, 1 });
			}
		}
		used_allocation_count = 0;

		for (size_t i = 0; i < available_allocation_count; i++) {
			auto& buf = available_allocations[i].buffer;
			if (buf) {
				upstream->deallocate_buffers(std::span{ &buf, 1 });
			}
		}

		available_allocation_count = 0;
	}

	Result<void, AllocateException> BufferSubAllocator::grow(size_t size, size_t alignment, SourceLocationAtFrame source) {
		BufferBlock alloc;
		BufferCreateInfo bci{ .mem_usage = mem_usage, .size = size, .alignment = alignment };
		auto result = upstream->allocate_buffers(std::span{ &alloc.buffer, 1 }, std::span{ &bci, 1 }, source);
		if (!result) {
			return result;
		}

		VmaVirtualBlockCreateInfo vbci{};
		vbci.size = size;
		auto result2 = vmaCreateVirtualBlock(&vbci, &alloc.block);
		if (!result2) {
			return { expected_error, AllocateException(result2) };
		}

		blocks.push_back(alloc);
		return { expected_value };
	}

	Result<Buffer, AllocateException> BufferSubAllocator::allocate_buffer(size_t size, size_t alignment, SourceLocationAtFrame source) {
		if (blocks.size() == 0) {
			auto result = grow(size, alignment, source);
			if (!result) {
				return result;
			}
		}
		VmaVirtualAllocationCreateInfo vaci{};
		vaci.size = size;
		vaci.alignment = alignment;

		VmaVirtualAllocation va;
		VkDeviceSize offset;

		auto result = vmaVirtualAllocate(blocks.back().block, &vaci, &va, &offset);
		if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
			auto result = grow(size, alignment, source);
			if (!result) {
				return result;
			}
		}
		// second try must succeed
		result = vmaVirtualAllocate(blocks.back().block, &vaci, &va, &offset);
		if (!result) {
			return { expected_error, AllocateException(result) };
		}

		Buffer buf = blocks.back().buffer.add_offset(offset);
		buf.allocation = new SubAllocation{ blocks.back().block, va };
		return { expected_value, buf };
	}

	void BufferSubAllocator::deallocate_buffer(const Buffer& buf) {
		auto sa = static_cast<SubAllocation*>(buf.allocation);
		vmaVirtualFree(sa->block, sa->allocation);
		delete sa;
	}

	void BufferSubAllocator::free() {
		for (auto& bb : blocks) {
			if (bb.buffer) {
				upstream->deallocate_buffers(std::span{ &bb.buffer, 1 });
				vmaDestroyVirtualBlock(bb.block);
			}
		}
		blocks.clear();
	}

	BufferSubAllocator::~BufferSubAllocator() {
		free();
	}

} // namespace vuk