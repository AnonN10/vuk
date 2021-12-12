#pragma once

#include "vuk/Allocator.hpp"
#include "vuk/resources/DeviceNestedResource.hpp"
#include "vuk/resources/DeviceVkResource.hpp"
#include "../src/LegacyGPUAllocator.hpp"

#include <atomic>

namespace vuk {
	struct DeviceSuperFrameResource;

	/// @brief Represents "per-frame" resources - temporary allocations that persist through a frame. Can only be used via the DeviceSuperFrameResource
	struct DeviceFrameResource : DeviceNestedResource {
		DeviceFrameResource(VkDevice device, DeviceSuperFrameResource& upstream);

		std::mutex sema_mutex;
		std::vector<VkSemaphore> semaphores;

		Result<void, AllocateException> allocate_semaphores(std::span<VkSemaphore> dst, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_semaphores(dst, loc));
			std::unique_lock _(sema_mutex);
			auto& vec = semaphores;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_semaphores(std::span<const VkSemaphore> src) override {} // noop

		std::mutex fence_mutex;
		std::vector<VkFence> fences;

		Result<void, AllocateException> allocate_fences(std::span<VkFence> dst, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_fences(dst, loc));
			std::unique_lock _(fence_mutex);
			auto& vec = fences;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_fences(std::span<const VkFence> src) override {} // noop

		std::mutex cbuf_mutex;
		std::vector<HLCommandBuffer> cmdbuffers_to_free;
		std::vector<VkCommandPool> cmdpools_to_free;

		// TODO: error propagation
		Result<void, AllocateException> allocate_hl_commandbuffers(std::span<HLCommandBuffer> dst, std::span<const HLCommandBufferCreateInfo> cis, SourceLocationAtFrame loc) override {
			std::unique_lock _(cbuf_mutex);
			assert(dst.size() == cis.size());
			auto cmdpools_size = cmdpools_to_free.size();
			cmdpools_to_free.resize(cmdpools_to_free.size() + dst.size());

			for (uint64_t i = 0; i < dst.size(); i++) {
				auto& ci = cis[i];
				VkCommandPoolCreateInfo cpci{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
				cpci.queueFamilyIndex = ci.queue_family_index;
				cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
				upstream->allocate_commandpools(std::span{ cmdpools_to_free.data() + cmdpools_size + i, 1 }, std::span{ &cpci, 1 }, loc);

				dst[i].command_pool = *(cmdpools_to_free.data() + cmdpools_size + i);
				VkCommandBufferAllocateInfo cbai{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
				cbai.commandBufferCount = 1;
				cbai.commandPool = dst[i].command_pool;
				cbai.level = ci.level;
				upstream->allocate_commandbuffers(std::span{ &dst[i].command_buffer, 1 }, std::span{ &cbai, 1 }, loc); // do not record cbuf, we deallocate it with the pool
			}

			return { expected_value };
		}

		void deallocate_hl_commandbuffers(std::span<const HLCommandBuffer> src) override {} // no-op, deallocated with pools

		Result<void, AllocateException> allocate_commandbuffers(std::span<VkCommandBuffer> dst, std::span<const VkCommandBufferAllocateInfo> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_commandbuffers(dst, cis, loc));
			std::unique_lock _(cbuf_mutex);
			cmdbuffers_to_free.reserve(cmdbuffers_to_free.size() + dst.size());
			for (uint64_t i = 0; i < dst.size(); i++) {
				cmdbuffers_to_free.emplace_back(dst[i], cis[i].commandPool);
			}
			return { expected_value };
		}

		void deallocate_commandbuffers(VkCommandPool pool, std::span<const VkCommandBuffer> dst) override {} // noop

		Result<void, AllocateException> allocate_commandpools(std::span<VkCommandPool> dst, std::span<const VkCommandPoolCreateInfo> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_commandpools(dst, cis, loc));
			std::unique_lock _(cbuf_mutex);
			auto& vec = cmdpools_to_free;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_commandpools(std::span<const VkCommandPool> dst) override {} // no-op

		// buffers are lockless
		Result<void, AllocateException> allocate_buffers(std::span<BufferCrossDevice> dst, std::span<const BufferCreateInfo> cis, SourceLocationAtFrame loc) override;

		void deallocate_buffers(std::span<const BufferCrossDevice> src) override {} // no-op, linear

		Result<void, AllocateException> allocate_buffers(std::span<BufferGPU> dst, std::span<const BufferCreateInfo> cis, SourceLocationAtFrame loc) override;

		void deallocate_buffers(std::span<const BufferGPU> src) override {} // no-op, linear

		std::mutex framebuffer_mutex;
		std::vector<VkFramebuffer> framebuffers;

		Result<void, AllocateException> allocate_framebuffers(std::span<VkFramebuffer> dst, std::span<const FramebufferCreateInfo> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_framebuffers(dst, cis, loc));
			std::unique_lock _(framebuffer_mutex);
			auto& vec = framebuffers;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_framebuffers(std::span<const VkFramebuffer> src) override {} // noop

		std::mutex images_mutex;
		std::vector<Image> images;

		Result<void, AllocateException> allocate_images(std::span<Image> dst, std::span<const ImageCreateInfo> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_images(dst, cis, loc));
			std::unique_lock _(images_mutex);
			auto& vec = images;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_images(std::span<const Image> src) override {} // noop

		std::mutex image_views_mutex;
		std::vector<ImageView> image_views;

		Result<void, AllocateException> allocate_image_views(std::span<ImageView> dst, std::span<const ImageViewCreateInfo> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_image_views(dst, cis, loc));
			std::unique_lock _(image_views_mutex);

			auto& vec = image_views;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_image_views(std::span<const ImageView> src) override {} // noop

		std::mutex pds_mutex;
		std::vector<PersistentDescriptorSet> persistent_descriptor_sets;

		Result<void, AllocateException> allocate_persistent_descriptor_sets(std::span<PersistentDescriptorSet> dst, std::span<const PersistentDescriptorSetCreateInfo> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_persistent_descriptor_sets(dst, cis, loc));
			std::unique_lock _(pds_mutex);

			auto& vec = persistent_descriptor_sets;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_persistent_descriptor_sets(std::span<const PersistentDescriptorSet> src) override {} // noop

		std::mutex ds_mutex;
		std::vector<DescriptorSet> descriptor_sets;

		Result<void, AllocateException> allocate_descriptor_sets(std::span<DescriptorSet> dst, std::span<const SetBinding> cis, SourceLocationAtFrame loc) override {
			VUK_DO_OR_RETURN(upstream->allocate_descriptor_sets(dst, cis, loc));

			std::unique_lock _(ds_mutex);

			auto& vec = descriptor_sets;
			vec.insert(vec.end(), dst.begin(), dst.end());
			return { expected_value };
		}

		void deallocate_descriptor_sets(std::span<const DescriptorSet> src) override {} // noop

		// only for use via SuperframeAllocator
		std::mutex buffers_mutex;
		std::vector<BufferGPU> buffer_gpus;
		std::vector<BufferCrossDevice> buffer_cross_devices;

		template<class T>
		struct LRUEntry {
			T value;
			size_t last_use_frame;
		};

		template<class T>
		struct Cache {
			robin_hood::unordered_map<create_info_t<T>, LRUEntry<T>> lru_map;
			std::array<std::vector<T>, 32> per_thread_append_v;
			std::array<std::vector<create_info_t<T>>, 32> per_thread_append_k;

			std::mutex cache_mtx;

			T& acquire(uint64_t current_frame, const create_info_t<T>& ci);
			void collect(uint64_t current_frame, size_t threshold);
		};

		Cache<DescriptorSet> descriptor_set_cache;

		void wait() {
			if (fences.size() > 0) {
				vkWaitForFences(device, (uint32_t)fences.size(), fences.data(), true, UINT64_MAX);
			}
		}

		Context& get_context() override {
			return upstream->get_context();
		}

		VkDevice device;
		uint64_t current_frame = 0;
		LegacyLinearAllocator linear_cpu_only;
		LegacyLinearAllocator linear_cpu_gpu;
		LegacyLinearAllocator linear_gpu_cpu;
		LegacyLinearAllocator linear_gpu_only;
	};

	/// @brief DeviceSuperFrameResource is an allocator that gives out DeviceFrameResource allocators, and manages their resources
	struct DeviceSuperFrameResource : DeviceResource {
		DeviceSuperFrameResource(Context& ctx, uint64_t frames_in_flight);

		std::unique_ptr<char[]> frames_storage;
		DeviceFrameResource* frames;

		DeviceFrameResource& get_last_frame() {
			return frames[frame_counter.load() % frames_in_flight];
		}

		Result<void, AllocateException> allocate_semaphores(std::span<VkSemaphore> dst, SourceLocationAtFrame loc) override {
			return direct.allocate_semaphores(dst, loc);
		}

		void deallocate_semaphores(std::span<const VkSemaphore> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.sema_mutex);
			auto& vec = get_last_frame().semaphores;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_fences(std::span<VkFence> dst, SourceLocationAtFrame loc) override {
			return direct.allocate_fences(dst, loc);
		}

		void deallocate_fences(std::span<const VkFence> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.fence_mutex);
			auto& vec = f.fences;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_commandbuffers(std::span<VkCommandBuffer> dst, std::span<const VkCommandBufferAllocateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_commandbuffers(dst, cis, loc);
		}

		void deallocate_commandbuffers(VkCommandPool pool, std::span<const VkCommandBuffer> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.cbuf_mutex);
			f.cmdbuffers_to_free.reserve(f.cmdbuffers_to_free.size() + src.size());
			for (auto& s : src) {
				f.cmdbuffers_to_free.emplace_back(s, pool);
			}
		}

		Result<void, AllocateException> allocate_hl_commandbuffers(std::span<HLCommandBuffer> dst, std::span<const HLCommandBufferCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_hl_commandbuffers(dst, cis, loc);
		}

		void deallocate_hl_commandbuffers(std::span<const HLCommandBuffer> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.cbuf_mutex);
			auto& vec = f.cmdpools_to_free;

			for (auto& s : src) {
				vec.push_back(s.command_pool);
			}
		}

		Result<void, AllocateException> allocate_commandpools(std::span<VkCommandPool> dst, std::span<const VkCommandPoolCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_commandpools(dst, cis, loc);
		}

		void deallocate_commandpools(std::span<const VkCommandPool> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.cbuf_mutex);
			auto& vec = f.cmdpools_to_free;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_buffers(std::span<BufferCrossDevice> dst, std::span<const BufferCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_buffers(dst, cis, loc);
		}

		void deallocate_buffers(std::span<const BufferCrossDevice> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.buffers_mutex);
			auto& vec = f.buffer_cross_devices;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_buffers(std::span<BufferGPU> dst, std::span<const BufferCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_buffers(dst, cis, loc);
		}

		void deallocate_buffers(std::span<const BufferGPU> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.buffers_mutex);
			auto& vec = f.buffer_gpus;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_framebuffers(std::span<VkFramebuffer> dst, std::span<const FramebufferCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_framebuffers(dst, cis, loc);
		}

		void deallocate_framebuffers(std::span<const VkFramebuffer> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.framebuffer_mutex);
			auto& vec = f.framebuffers;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_images(std::span<Image> dst, std::span<const ImageCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_images(dst, cis, loc);
		}

		void deallocate_images(std::span<const Image> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.images_mutex);
			auto& vec = f.images;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_image_views(std::span<ImageView> dst, std::span<const ImageViewCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_image_views(dst, cis, loc);
		}

		void deallocate_image_views(std::span<const ImageView> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.image_views_mutex);
			auto& vec = f.image_views;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_persistent_descriptor_sets(std::span<PersistentDescriptorSet> dst, std::span<const PersistentDescriptorSetCreateInfo> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_persistent_descriptor_sets(dst, cis, loc);
		}

		void deallocate_persistent_descriptor_sets(std::span<const PersistentDescriptorSet> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.pds_mutex);
			auto& vec = f.persistent_descriptor_sets;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		Result<void, AllocateException> allocate_descriptor_sets(std::span<DescriptorSet> dst, std::span<const SetBinding> cis, SourceLocationAtFrame loc) override {
			return direct.allocate_descriptor_sets(dst, cis, loc);
		}

		void deallocate_descriptor_sets(std::span<const DescriptorSet> src) override {
			auto& f = get_last_frame();
			std::unique_lock _(f.ds_mutex);
			auto& vec = f.descriptor_sets;
			vec.insert(vec.end(), src.begin(), src.end());
		}

		DeviceFrameResource& get_next_frame();

		void deallocate_frame(DeviceFrameResource& f) {
			//f.descriptor_set_cache.collect(frame_counter.load(), 16);
			direct.deallocate_semaphores(f.semaphores);
			direct.deallocate_fences(f.fences);
			for (auto& c : f.cmdbuffers_to_free) {
				direct.deallocate_commandbuffers(c.command_pool, std::span{ &c.command_buffer, 1 });
			}
			direct.deallocate_commandpools(f.cmdpools_to_free);
			direct.deallocate_buffers(f.buffer_gpus);
			direct.deallocate_buffers(f.buffer_cross_devices);
			direct.deallocate_framebuffers(f.framebuffers);
			direct.deallocate_images(f.images);
			direct.deallocate_image_views(f.image_views);
			direct.deallocate_persistent_descriptor_sets(f.persistent_descriptor_sets);
			direct.deallocate_descriptor_sets(f.descriptor_sets);

			f.semaphores.clear();
			f.fences.clear();
			f.buffer_cross_devices.clear();
			f.buffer_gpus.clear();
			f.cmdbuffers_to_free.clear();
			f.cmdpools_to_free.clear();
			auto& legacy = direct.legacy_gpu_allocator;
			legacy->reset_pool(f.linear_cpu_only);
			legacy->reset_pool(f.linear_cpu_gpu);
			legacy->reset_pool(f.linear_gpu_cpu);
			legacy->reset_pool(f.linear_gpu_only);
			f.framebuffers.clear();
			f.images.clear();
			f.image_views.clear();
			f.persistent_descriptor_sets.clear();
			f.descriptor_sets.clear();
		}

		virtual ~DeviceSuperFrameResource() {
			for (auto i = 0; i < frames_in_flight; i++) {
				auto lframe = (frame_counter + i) % frames_in_flight;
				auto& f = frames[lframe];
				f.wait();
				deallocate_frame(f);
				direct.legacy_gpu_allocator->destroy(f.linear_cpu_only);
				direct.legacy_gpu_allocator->destroy(f.linear_cpu_gpu);
				direct.legacy_gpu_allocator->destroy(f.linear_gpu_cpu);
				direct.legacy_gpu_allocator->destroy(f.linear_gpu_only);
				f.~DeviceFrameResource();
			}
		}

		Context& get_context() override {
			return *direct.ctx;
		}

		DeviceVkResource direct;
		std::mutex new_frame_mutex;
		std::atomic<uint64_t> frame_counter;
		std::atomic<uint64_t> local_frame;
		const uint64_t frames_in_flight;
	};
}