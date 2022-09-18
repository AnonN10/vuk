#include "TestContext.hpp"
#include "vuk/AllocatorHelpers.hpp"
#include "vuk/Partials.hpp"
#include <doctest/doctest.h>
#include <numeric>
#include <sstream>
#include <fstream>

using namespace vuk;

constexpr bool operator==(const std::span<uint32_t>& lhs, const std::span<uint32_t>& rhs) {
	return std::equal(begin(lhs), end(lhs), begin(rhs), end(rhs));
}

constexpr bool operator==(const std::span<uint32_t>& lhs, const std::span<const uint32_t>& rhs) {
	return std::equal(begin(lhs), end(lhs), begin(rhs), end(rhs));
}

constexpr bool operator==(const std::span<const uint32_t>& lhs, const std::span<const uint32_t>& rhs) {
	return std::equal(begin(lhs), end(lhs), begin(rhs), end(rhs));
}

constexpr bool operator==(const std::span<float>& lhs, const std::span<float>& rhs) {
	return std::equal(begin(lhs), end(lhs), begin(rhs), end(rhs));
}

constexpr bool operator==(const std::span<float>& lhs, const std::span<const float>& rhs) {
	return std::equal(begin(lhs), end(lhs), begin(rhs), end(rhs));
}

constexpr bool operator==(const std::span<const float>& lhs, const std::span<const float>& rhs) {
	return std::equal(begin(lhs), end(lhs), begin(rhs), end(rhs));
}

struct CountWithIndirect {
	CountWithIndirect(uint32_t count, uint32_t wg_size) : workgroup_count((uint32_t)idivceil(count, wg_size)), count(count) {}

	uint32_t workgroup_count;
	uint32_t yz[2] = { 1, 1 };
	uint32_t count;
};

inline std::string read_entire_file(const std::string& path) {
	std::ostringstream buf;
	std::ifstream input(path.c_str());
	assert(input);
	buf << input.rdbuf();
	return buf.str();
}

PipelineBaseInfo* static_compute_pbi(Context& ctx, std::string src, std::string ident) {
	vuk::PipelineBaseCreateInfo pci;
	pci.add_glsl(src, std::move(ident));
	/* FILE* fo = fopen("dumb.spv", "wb");
	fwrite(ptr, sizeof(uint32_t), size, fo);
	fclose(fo);*/
	return ctx.get_pipeline(pci);
}

template<class T, class F>
inline Future scan(Context& ctx, Future src, Future dst, Future count, uint32_t max_size, const F& fn) {
	static auto pbi_u = static_compute_pbi(ctx, read_entire_file("../../include/vuk/partials/shaders/blelloch_scan.comp"), "scan");
	static auto pbi_a = static_compute_pbi(ctx, read_entire_file("../../include/vuk/partials/shaders/blelloch_add.comp"), "add");
	std::shared_ptr<RenderGraph> rgp = std::make_shared<RenderGraph>("scan");
	rgp->attach_in("src", std::move(src));
	if (dst) {
		rgp->attach_in("dst", std::move(dst));
	} else {
		rgp->attach_buffer("dst", Buffer{ .memory_usage = vuk::MemoryUsage::eGPUonly });
		rgp->inference_rule("dst", same_size_as("src"));
	}
	rgp->attach_in("count", std::move(count));
	rgp->attach_buffer("temp", Buffer{ .size = 2 * 128 * 4, .memory_usage = vuk::MemoryUsage::eGPUonly });
	rgp->add_pass({ .name = "scan",
	                .resources = { "src"_buffer >> eComputeRead,
	                               "dst"_buffer >> eComputeWrite,
	                               "temp"_buffer >> eComputeWrite,
	                               "count"_buffer >> eComputeRW,
	                               "count"_buffer >> eIndirectRead },
	                .execute = [](CommandBuffer& command_buffer) {
		                command_buffer.bind_buffer(0, 0, "src");
		                command_buffer.bind_buffer(0, 1, "dst");
		                command_buffer.bind_buffer(0, 2, "temp");
		                command_buffer.bind_buffer(0, 4, "count");
		                command_buffer.bind_compute_pipeline(pbi_u);
		                command_buffer.dispatch_indirect("count");
	                } });
	rgp->add_pass({ .name = "add",
	                .resources = { "dst+"_buffer >> eComputeRW, "temp+"_buffer >> eComputeRead, "count+"_buffer >> eComputeRead, "count+"_buffer >> eIndirectRead },
	                .execute = [](CommandBuffer& command_buffer) {
		                command_buffer.bind_buffer(0, 0, "src");
		                command_buffer.bind_buffer(0, 1, "dst+");
		                command_buffer.bind_buffer(0, 2, "temp+");
		                command_buffer.bind_buffer(0, 4, "count");
		                command_buffer.bind_compute_pipeline(pbi_a);
		                command_buffer.dispatch_indirect("count");
	                } });

	return { rgp, "dst++" };
}

TEST_CASE("test scan") {
	REQUIRE(test_context.prepare());
	{
		if (test_context.rdoc_api)
			test_context.rdoc_api->StartFrameCapture(NULL, NULL);
		// src data
		std::vector<unsigned> data(128 * 65);
		std::iota(data.begin(), data.end(), 0);
		// function to apply
		auto func = [](auto A, auto B) {
			return A + B;
		};
		std::vector<uint32_t> expected;
		// cpu result
		std::exclusive_scan(data.begin(), data.end(), std::back_inserter(expected), 0, func);

		// put data on gpu
		auto [_1, src] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(data));
		// put count on gpu
		CountWithIndirect count_data{ (uint32_t)data.size(), 128 };
		auto [_2, cnt] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(&count_data, 1));

		// apply function on gpu
		auto calc = scan<uint32_t>(*test_context.context, src, {}, cnt, 3, func);
		// bring data back to cpu
		auto res = download_buffer(calc).get<Buffer>(*test_context.allocator, test_context.compiler);
		auto out = std::span((uint32_t*)res->mapped_ptr, data.size());
		if (test_context.rdoc_api)
			test_context.rdoc_api->EndFrameCapture(NULL, NULL);
		CHECK(out == std::span(expected));
	}
}