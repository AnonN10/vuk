#include "TestContext.hpp"
#include "vuk/AllocatorHelpers.hpp"
#include "vuk/Partials.hpp"
#include <doctest/doctest.h>

using namespace vuk;

TEST_CASE("buffer harness") {
	auto data = { 1u, 2u, 3u };
	auto [buf, fut] = create_buffer(*test_context.allocator, MemoryUsage::eCPUtoGPU, vuk::DomainFlagBits::eTransferOnTransfer, std::span(data));
	auto res = fut.get(*test_context.allocator, test_context.compiler);
	CHECK(std::span((uint32_t*)res->mapped_ptr, 3) == std::span(data));
}

TEST_CASE("buffer upload/download") {
	{
		auto data = { 1u, 2u, 3u };
		auto [buf, fut] = create_buffer(*test_context.allocator, MemoryUsage::eGPUonly, DomainFlagBits::eAny, std::span(data));

		auto res = download_buffer(fut).get(*test_context.allocator, test_context.compiler);
		CHECK(std::span((uint32_t*)res->mapped_ptr, 3) == std::span(data));
	}
	{
		auto data = { 1u, 2u, 3u, 4u, 5u };
		auto [buf, fut] = create_buffer(*test_context.allocator, MemoryUsage::eGPUonly, DomainFlagBits::eAny, std::span(data));

		auto res = download_buffer(fut).get(*test_context.allocator, test_context.compiler);
		CHECK(std::span((uint32_t*)res->mapped_ptr, 5) == std::span(data));
	}
}

TEST_CASE("buffer fill & update") {
	{
		auto data = { 0xfeu, 0xfeu, 0xfeu, 0xfeu };
		auto buf = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });

		auto fill = make_pass("fill", [](CommandBuffer& cbuf, VUK_BA(Access::eTransferWrite) dst) {
			cbuf.fill_buffer(dst, 0xfe);
			return dst;
		});

		auto res = download_buffer(fill(declare_buf("src", **buf))).get(*test_context.allocator, test_context.compiler);
		CHECK(std::span((uint32_t*)res->mapped_ptr, 4) == std::span(data));
	}
	{
		std::array<const uint32_t, 4> data = { 0xfeu, 0xfeu, 0xfeu, 0xfeu };
		auto buf = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });

		auto fill = make_pass("update", [data](CommandBuffer& cbuf, VUK_BA(Access::eTransferWrite) dst) {
			cbuf.update_buffer(dst, &data[0]);
			return dst;
		});

		auto res = download_buffer(fill(declare_buf("src", **buf))).get(*test_context.allocator, test_context.compiler);
		CHECK(std::span((uint32_t*)res->mapped_ptr, 4) == std::span(data));
	}
}

auto image2buf = make_pass("copy image to buffer", [](CommandBuffer& cbuf, VUK_IA(Access::eTransferRead) src, VUK_BA(Access::eTransferWrite) dst) {
	BufferImageCopy bc;
	bc.imageOffset = { 0, 0, 0 };
	bc.bufferRowLength = 0;
	bc.bufferImageHeight = 0;
	bc.imageExtent = static_cast<Extent3D>(src->extent.extent);
	bc.imageSubresource.aspectMask = format_to_aspect(src->format);
	bc.imageSubresource.mipLevel = src->base_level;
	bc.imageSubresource.baseArrayLayer = src->base_layer;
	assert(src->layer_count == 1); // unsupported yet
	bc.imageSubresource.layerCount = src->layer_count;
	bc.bufferOffset = dst->offset;
	cbuf.copy_image_to_buffer(src, dst, bc);
	return dst;
});

TEST_CASE("image upload/download") {
	{
		auto data = { 1u, 2u, 3u, 4u };
		auto ia = ImageAttachment::from_preset(ImageAttachment::Preset::eGeneric2D, Format::eR32Uint, { 2, 2, 1 }, Samples::e1);
		auto [img, fut] = create_image_with_data(*test_context.allocator, DomainFlagBits::eAny, ia, std::span(data));

		size_t alignment = format_to_texel_block_size(fut->format);
		assert(fut->extent.sizing == Sizing::eAbsolute);
		size_t size = compute_image_size(fut->format, static_cast<Extent3D>(fut->extent.extent));
		auto dst = *allocate_buffer(*test_context.allocator, BufferCreateInfo{ MemoryUsage::eCPUonly, size, alignment });
		auto dst_buf = declare_buf("dst", *dst);
		auto res = download_buffer(image2buf(fut, std::move(dst_buf))).get(*test_context.allocator, test_context.compiler);
		auto updata = std::span((uint32_t*)res->mapped_ptr, 4);
		CHECK(updata == std::span(data));
	}
}

TEST_CASE("image clear") {
	{
		auto data = { 1u, 2u, 3u, 4u };
		auto ia = ImageAttachment::from_preset(ImageAttachment::Preset::eGeneric2D, Format::eR32Uint, { 2, 2, 1 }, Samples::e1);
		auto [img, fut] = create_image_with_data(*test_context.allocator, DomainFlagBits::eAny, ia, std::span(data));

		size_t alignment = format_to_texel_block_size(fut->format);
		assert(fut->extent.sizing == Sizing::eAbsolute);
		size_t size = compute_image_size(fut->format, static_cast<Extent3D>(fut->extent.extent));
		auto dst = *allocate_buffer(*test_context.allocator, BufferCreateInfo{ MemoryUsage::eCPUonly, size, alignment });
		auto fut2 = clear_image(fut, vuk::ClearColor(5u, 5u, 5u, 5u));
		auto dst_buf = declare_buf("dst", *dst);
		auto res = download_buffer(image2buf(fut2, dst_buf)).get(*test_context.allocator, test_context.compiler);
		auto updata = std::span((uint32_t*)res->mapped_ptr, 4);
		CHECK(std::all_of(updata.begin(), updata.end(), [](auto& elem) { return elem == 5; }));
	}
}

TEST_CASE("image blit") {
	{
		auto data = { 1.f, 0.f, 0.f, 1.f };
		auto ia_src = ImageAttachment::from_preset(ImageAttachment::Preset::eGeneric2D, Format::eR32Sfloat, { 2, 2, 1 }, Samples::e1);
		ia_src.level_count = 1;
		auto [img, fut] = create_image_with_data(*test_context.allocator, DomainFlagBits::eAny, ia_src, std::span(data));
		auto ia_dst = ImageAttachment::from_preset(ImageAttachment::Preset::eGeneric2D, Format::eR32Sfloat, { 1, 1, 1 }, Samples::e1);
		ia_dst.level_count = 1;
		auto img2 = allocate_image(*test_context.allocator, ia_dst);
		size_t alignment = format_to_texel_block_size(fut->format);
		assert(fut->extent.sizing == Sizing::eAbsolute);
		size_t size = compute_image_size(fut->format, static_cast<Extent3D>(fut->extent.extent));
		auto dst = *allocate_buffer(*test_context.allocator, BufferCreateInfo{ MemoryUsage::eCPUonly, size, alignment });
		auto fut2 = blit_image(fut, declare_ia("dst_i", ia_dst), Filter::eLinear);
		auto dst_buf = declare_buf("dst", *dst);
		auto res = download_buffer(image2buf(fut2, dst_buf)).get(*test_context.allocator, test_context.compiler);
		auto updata = std::span((float*)res->mapped_ptr, 1);
		CHECK(std::all_of(updata.begin(), updata.end(), [](auto& elem) { return elem == 0.5f; }));
	}
	{
		auto data = { 1.f, 0.f, 0.f, 1.f };
		auto ia_src = ImageAttachment::from_preset(ImageAttachment::Preset::eGeneric2D, Format::eR32Sfloat, { 2, 2, 1 }, Samples::e1);
		ia_src.level_count = 1;
		auto [img, fut] = create_image_with_data(*test_context.allocator, DomainFlagBits::eAny, ia_src, std::span(data));
		auto ia_dst = ImageAttachment::from_preset(ImageAttachment::Preset::eGeneric2D, Format::eR32Sfloat, { 1, 1, 1 }, Samples::e1);
		ia_dst.level_count = 1;
		auto img2 = allocate_image(*test_context.allocator, ia_dst);
		size_t alignment = format_to_texel_block_size(fut->format);
		assert(fut->extent.sizing == Sizing::eAbsolute);
		size_t size = compute_image_size(fut->format, static_cast<Extent3D>(fut->extent.extent));
		auto dst = *allocate_buffer(*test_context.allocator, BufferCreateInfo{ MemoryUsage::eCPUonly, size, alignment });
		auto fut2 = blit_image(fut, declare_ia("dst_i", ia_dst), Filter::eNearest);
		auto dst_buf = declare_buf("dst", *dst);
		auto res = download_buffer(image2buf(fut2, dst_buf)).get(*test_context.allocator, test_context.compiler);
		auto updata = std::span((float*)res->mapped_ptr, 1);
		CHECK(std::all_of(updata.begin(), updata.end(), [](auto& elem) { return elem == 1.f; }));
	}
}

TEST_CASE("multi return pass") {
	{
		auto buf0 = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });
		auto buf1 = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });
		auto buf2 = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });

		auto fills = make_pass(
		    "fills", [](CommandBuffer& cbuf, VUK_BA(Access::eTransferWrite) dst0, VUK_BA(Access::eTransferWrite) dst1, VUK_BA(Access::eTransferWrite) dst2) {
			    cbuf.fill_buffer(dst0, 0xfc);
			    cbuf.fill_buffer(dst1, 0xfd);
			    cbuf.fill_buffer(dst2, 0xfe);
			    return std::tuple{ dst0, dst1, dst2 };
		    });

		auto [buf0p, buf1p, buf2p] = fills(declare_buf("src0", **buf0), declare_buf("src1", **buf1), declare_buf("src2", **buf2));
		{
			auto data = { 0xfcu, 0xfcu, 0xfcu, 0xfcu };
			auto res = download_buffer(buf0p).get(*test_context.allocator, test_context.compiler);
			CHECK(std::span((uint32_t*)res->mapped_ptr, 4) == std::span(data));
		}
		{
			auto data = { 0xfdu, 0xfdu, 0xfdu, 0xfdu };
			auto res = download_buffer(buf1p).get(*test_context.allocator, test_context.compiler);
			CHECK(std::span((uint32_t*)res->mapped_ptr, 4) == std::span(data));
		}
		{
			auto data = { 0xfeu, 0xfeu, 0xfeu, 0xfeu };
			auto res = download_buffer(buf2p).get(*test_context.allocator, test_context.compiler);
			CHECK(std::span((uint32_t*)res->mapped_ptr, 4) == std::span(data));
		}
	}
}

#include <string>

TEST_CASE("scheduling single-queue") {
	{
		std::string execution;
		
		auto buf0 = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });
		
		auto write = make_pass("write", [&](CommandBuffer& cbuf, VUK_BA(Access::eTransferWrite) dst) {
			execution += "w";
			    return dst;
		    });
		auto read = make_pass("read", [&](CommandBuffer& cbuf, VUK_BA(Access::eTransferRead) dst) {
			execution += "r";
			return dst;
		});

		{
			auto b0 = declare_buf("src0", **buf0);
			write(write(b0)).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "ww");
			execution = "";
		}
		{
			auto b0 = declare_buf("src0", **buf0);
			read(write(b0)).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "wr");
			execution = "";
		}
		{
			auto b0 = declare_buf("src0", **buf0);
			write(read(write(b0))).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "wrw");
			execution = "";
		}
		{
			auto b0 = declare_buf("src0", **buf0);
			write(read(read(write(b0)))).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "wrrw");
		}
	}
}

TEST_CASE("scheduling with submitted") {
	{
		std::string execution;

		auto buf0 = allocate_buffer(*test_context.allocator, { .mem_usage = MemoryUsage::eGPUonly, .size = sizeof(uint32_t) * 4 });

		auto write = make_pass("write", [&](CommandBuffer& cbuf, VUK_BA(Access::eTransferWrite) dst) {
			execution += "w";
			return dst;
		});
		auto read = make_pass("read", [&](CommandBuffer& cbuf, VUK_BA(Access::eTransferRead) dst) {
			execution += "r";
			return dst;
		});

		{
			auto written = write(declare_buf("src0", **buf0));
			written.wait(*test_context.allocator, test_context.compiler);
			read(written).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "wr");
			execution = "";
		}
		{
			auto written = write(declare_buf("src0", **buf0));
			written.wait(*test_context.allocator, test_context.compiler);
			read(std::move(written)).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "wr");
			execution = "";
		}
		{
			auto written = write(declare_buf("src0", **buf0));
			written.wait(*test_context.allocator, test_context.compiler);
			write(std::move(written)).wait(*test_context.allocator, test_context.compiler);
			CHECK(execution == "ww");
			execution = "";
		}
	}
}

// TEST TODOS: image2image copy, resolve