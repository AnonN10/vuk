#include "vuk/Context.hpp"
#include "vuk/RenderGraph.hpp"
#include "vuk/AllocatorHelpers.hpp"

namespace vuk {
	Result<void> execute_submit(Allocator& allocator, ExecutableRenderGraph&& rg, std::vector<std::pair<SwapchainRef, size_t>> swapchains_with_indexes, VkSemaphore present_rdy, VkSemaphore render_complete) {
		Context& ctx = allocator.get_context();
		auto sbundle = rg.execute(allocator, swapchains_with_indexes);
		if (!sbundle) {
			return { expected_error, sbundle.error() };
		}

		auto domain_to_queue_index = [](DomainFlagBits domain) -> uint64_t {
			auto queue_only = (DomainFlagBits)(domain & DomainFlagBits::eQueueMask).m_mask;
			switch (queue_only) {
			case DomainFlagBits::eGraphicsQueue:
				return 0;
			case DomainFlagBits::eComputeQueue:
				return 1;
			case DomainFlagBits::eTransferQueue:
				return 2;
			default:
				assert(0);
				return 0;
			}
		};

		auto domain_to_queue = [](Context& ctx, DomainFlagBits domain) -> Queue& {
			auto queue_only = (DomainFlagBits)(domain & DomainFlagBits::eQueueMask).m_mask;
			switch (queue_only) {
			case DomainFlagBits::eGraphicsQueue:
				return *ctx.graphics_queue;
			case DomainFlagBits::eComputeQueue:
				return *ctx.compute_queue;
			case DomainFlagBits::eTransferQueue:
				return *ctx.transfer_queue;
			default:
				assert(0);
				return *ctx.transfer_queue;
			}
		};

		vuk::DomainFlags used_domains;
		for (auto& batch : sbundle->batches) {
			used_domains |= batch.domain;
		}

		std::array<uint64_t, 3> queue_progress_references;
		std::unique_lock<std::mutex> gfx_lock;
		if (used_domains & DomainFlagBits::eGraphicsQueue) {
			queue_progress_references[domain_to_queue_index(DomainFlagBits::eGraphicsQueue)] = *ctx.graphics_queue->submit_sync.value;
			gfx_lock = std::unique_lock{ ctx.graphics_queue->queue_lock };
		}
		std::unique_lock<std::mutex> compute_lock;
		if (used_domains & DomainFlagBits::eComputeQueue) {
			queue_progress_references[domain_to_queue_index(DomainFlagBits::eComputeQueue)] = *ctx.compute_queue->submit_sync.value;
			compute_lock = std::unique_lock{ ctx.compute_queue->queue_lock };
		}
		std::unique_lock<std::mutex> transfer_lock;
		if (used_domains & DomainFlagBits::eTransferQueue) {
			queue_progress_references[domain_to_queue_index(DomainFlagBits::eTransferQueue)] = *ctx.transfer_queue->submit_sync.value;
			transfer_lock = std::unique_lock{ ctx.transfer_queue->queue_lock };
		}

		for (SubmitBatch& batch : sbundle->batches) {
			auto domain = batch.domain;
			Queue& queue = domain_to_queue(ctx, domain);
			for (uint64_t i = 0; i < batch.submits.size(); i++) {
				SubmitInfo& submit_info = batch.submits[i];
				Unique<VkFence> fence(allocator);
				VUK_DO_OR_RETURN(allocator.allocate_fences({ &*fence, 1 }));

				VkSubmitInfo2KHR si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR };
				si.commandBufferInfoCount = (uint32_t)submit_info.command_buffers.size();
				std::vector<VkCommandBufferSubmitInfoKHR> cbufsis(si.commandBufferInfoCount);
				for (uint64_t i = 0; i < si.commandBufferInfoCount; i++) {
					cbufsis[i] = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR, .commandBuffer = submit_info.command_buffers[i] };
				}
				si.pCommandBufferInfos = cbufsis.data();

				std::vector<VkSemaphoreSubmitInfoKHR> wait_semas;
				for (auto& w : submit_info.relative_waits) {
					VkSemaphoreSubmitInfoKHR ssi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR };
					auto& wait_queue = domain_to_queue(ctx, w.first).submit_sync;
					ssi.semaphore = wait_queue.semaphore;
					ssi.value = queue_progress_references[domain_to_queue_index(w.first)] + w.second;
					ssi.stageMask = (VkPipelineStageFlagBits2KHR)PipelineStageFlagBits::eAllCommands;
					wait_semas.emplace_back(ssi);
				}
				if (domain == DomainFlagBits::eGraphicsQueue && i == 0 && present_rdy != VK_NULL_HANDLE) { // TODO: for first cbuf only that refs the swapchain attment
					VkSemaphoreSubmitInfoKHR ssi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR };
					ssi.semaphore = present_rdy;
					ssi.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR;
					//wait_semas.emplace_back(ssi);
				}
				si.pWaitSemaphoreInfos = wait_semas.data();
				si.waitSemaphoreInfoCount = (uint32_t)wait_semas.size();

				std::vector<VkSemaphoreSubmitInfoKHR> signal_semas;
				VkSemaphoreSubmitInfoKHR ssi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR };
				ssi.semaphore = queue.submit_sync.semaphore;
				ssi.value = ++ * queue.submit_sync.value;

				ssi.stageMask = (VkPipelineStageFlagBits2KHR)PipelineStageFlagBits::eAllCommands;
				signal_semas.emplace_back(ssi);
				if (domain == DomainFlagBits::eGraphicsQueue && i == batch.submits.size() - 1 && render_complete != VK_NULL_HANDLE) { // TODO: for final cbuf only that refs the swapchain attment
					ssi.semaphore = render_complete;
					ssi.value = 0; // binary sema
					signal_semas.emplace_back(ssi);
				}
				si.pSignalSemaphoreInfos = signal_semas.data();
				si.signalSemaphoreInfoCount = (uint32_t)signal_semas.size();
				VUK_DO_OR_RETURN(queue.submit(std::span{ &si, 1 }, *fence));

				for (auto& fut : submit_info.future_signals) {
					fut->status = FutureBase::Status::eSubmitted;
				}
			}
		}

		return { expected_value };
	}

	Result<void> execute_submit_and_present_to_one(Allocator& allocator, ExecutableRenderGraph&& rg, SwapchainRef swapchain) {
		Context& ctx = allocator.get_context();
		Unique<std::array<VkSemaphore, 2>> semas(allocator);
		VUK_DO_OR_RETURN(allocator.allocate_semaphores(*semas));
		auto [present_rdy, render_complete] = *semas;

		uint32_t image_index = (uint32_t)-1;
		VkResult acq_result = vkAcquireNextImageKHR(ctx.device, swapchain->swapchain, UINT64_MAX, present_rdy, VK_NULL_HANDLE, &image_index);
		if (acq_result != VK_SUCCESS) {
			VkSubmitInfo si{ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
			si.commandBufferCount = 0;
			si.pCommandBuffers = nullptr;
			si.waitSemaphoreCount = 1;
			si.pWaitSemaphores = &present_rdy;
			VkPipelineStageFlags flags = (VkPipelineStageFlags)PipelineStageFlagBits::eTopOfPipe;
			si.pWaitDstStageMask = &flags;
			VUK_DO_OR_RETURN(ctx.submit_graphics(std::span{ &si, 1 }, VK_NULL_HANDLE));
			return { expected_error, PresentException{acq_result} };
		}

		std::vector<std::pair<SwapchainRef, size_t>> swapchains_with_indexes = { { swapchain, image_index } };

		VUK_DO_OR_RETURN(execute_submit(allocator, std::move(rg), swapchains_with_indexes, present_rdy, render_complete));

		VkPresentInfoKHR pi{ .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain->swapchain;
		pi.pImageIndices = &image_index;
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &render_complete;
		auto present_result = vkQueuePresentKHR(ctx.graphics_queue->queue, &pi);
		if (present_result != VK_SUCCESS) {
			return { expected_error, PresentException{present_result} };
		}

		return { expected_value };
	}

	Result<void> execute_submit_and_wait(Allocator& allocator, ExecutableRenderGraph&& rg) {
		Context& ctx = allocator.get_context();

		assert(0);
		return { expected_value };
	}

	SampledImage make_sampled_image(ImageView iv, SamplerCreateInfo sci) {
		return { SampledImage::Global{ iv, sci, ImageLayout::eShaderReadOnlyOptimal } };
	}

	SampledImage make_sampled_image(Name n, SamplerCreateInfo sci) {
		return{ SampledImage::RenderGraphAttachment{ n, sci, {}, ImageLayout::eShaderReadOnlyOptimal } };
	}

	SampledImage make_sampled_image(Name n, ImageViewCreateInfo ivci, SamplerCreateInfo sci) {
		return { SampledImage::RenderGraphAttachment{ n, sci, ivci, ImageLayout::eShaderReadOnlyOptimal } };
	}

	Unique<ImageView>::~Unique() noexcept {
		if (allocator && payload.payload != VK_NULL_HANDLE) {
			deallocate(*allocator, payload);
		}
	}

	void Unique<ImageView>::reset(ImageView value) noexcept {
		if (payload != value) {
			if (allocator && payload != ImageView{}) {
				deallocate(*allocator, std::move(payload));
			}
			payload = std::move(value);
		}
	}

	FutureBase::FutureBase(Allocator& alloc) : allocator(&alloc) {}

	template<class T>
	Future<T>::Future(Allocator& alloc, struct RenderGraph& rg, Name output_binding) : control(std::make_unique<FutureBase>(alloc)), rg(&rg), output_binding(output_binding) {
		control->status = FutureBase::Status::eRenderGraphBound;
		this->rg->attach_out(output_binding, *this);
	}

	template<class T>
	Future<T>::Future(Allocator& alloc, std::unique_ptr<struct RenderGraph> org, Name output_binding) : control(std::make_unique<FutureBase>(alloc)), owned_rg(std::move(org)), rg(owned_rg.get()), output_binding(output_binding) {
		control->status = FutureBase::Status::eRenderGraphBound;
		rg->attach_out(output_binding, *this);
	}

	template<class T>
	Future<T>::Future(Allocator& alloc, T&& value) : control(std::make_unique<FutureBase>(alloc)) {
		control->get_result<T>() = std::move(value);
		control->status = FutureBase::Status::eHostAvailable;
	}

	template<class T>
	Result<T> Future<T>::get() {
		if (control->status == FutureBase::Status::eInputAttached || control->status == FutureBase::Status::eInitial) {
			return { expected_error }; // can't get result of future that has not been attached anything or has been attached into a rendergraph
		} else if (control->status == FutureBase::Status::eHostAvailable) {
			return { expected_value, control->get_result<T>()};
		} else if (control->status == FutureBase::Status::eSubmitted) {
			// TODO:
			//allocator->get_context().wait_for_queues();
			return { expected_value, control->get_result<T>() };
		} else {
			VUK_DO_OR_RETURN(execute_submit(*control->allocator, std::move(*rg).link(control->allocator->get_context(), {}), {}, {}, {}));
			control->allocator->get_context().wait_idle(); // TODO:
			control->status = FutureBase::Status::eHostAvailable;
			return { expected_value, control->get_result<T>() };
		}
	}

	template<class T>
	Result<void> Future<T>::submit() {
		if (control->status == FutureBase::Status::eInputAttached || control->status == FutureBase::Status::eInitial) {
			return { expected_error };
		} else if (control->status == FutureBase::Status::eHostAvailable || control->status == FutureBase::Status::eSubmitted) {
			return { expected_value }; // nothing to do
		} else {
			control->status = FutureBase::Status::eSubmitted;
			VUK_DO_OR_RETURN(execute_submit(*control->allocator, std::move(*rg).link(control->allocator->get_context(), {}), {}, {}, {}));
			return { expected_value };
		}
	}

	template struct Future<ImageAttachment>;
	template struct Future<Buffer>;
}