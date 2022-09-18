#include "TestContext.hpp"
#include "vuk/AllocatorHelpers.hpp"
#include "vuk/Partials.hpp"
#include "vuk/SPIRVTemplate.hpp"
#include <doctest/doctest.h>

using namespace vuk;

TEST_CASE("test text_context preparation") {
	REQUIRE(test_context.prepare());
}

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

TEST_CASE("test buffer harness") {
	REQUIRE(test_context.prepare());
	auto data = { 1u, 2u, 3u };
	auto [buf, fut] = create_buffer_cross_device(*test_context.allocator, MemoryUsage::eCPUtoGPU, std::span(data));
	auto res = fut.get<Buffer>(*test_context.allocator, test_context.compiler);
	CHECK(std::span((uint32_t*)res->mapped_ptr, 3) == std::span(data));
}

TEST_CASE("test buffer upload/download") {
	REQUIRE(test_context.prepare());
	{
		auto data = { 1u, 2u, 3u };
		auto [buf, fut] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(data));

		auto res = download_buffer(fut).get<Buffer>(*test_context.allocator, test_context.compiler);
		CHECK(std::span((uint32_t*)res->mapped_ptr, 3) == std::span(data));
	}
	{
		auto data = { 1u, 2u, 3u, 4u, 5u };
		auto [buf, fut] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(data));

		auto res = download_buffer(fut).get<Buffer>(*test_context.allocator, test_context.compiler);
		CHECK(std::span((uint32_t*)res->mapped_ptr, 5) == std::span(data));
	}
}

TEST_CASE("test unary map") {
	REQUIRE(test_context.prepare());
	{
		if (test_context.rdoc_api)
			test_context.rdoc_api->StartFrameCapture(NULL, NULL);
		// src data
		std::vector data = { 1u, 2u, 3u };
		// function to apply
		auto func = [](auto A) {
			return A + 3u + 33u;
		};
		std::vector<uint32_t> expected;
		// cpu result
		std::transform(data.begin(), data.end(), std::back_inserter(expected), func);

		// put data on gpu
		auto [_1, src] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(data));
		// put count on gpu
		CountWithIndirect count_data{ (uint32_t)data.size(), 64 };
		auto [_2, cnt] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(&count_data, 1));

		// apply function on gpu
		auto calc = unary_map<uint32_t>(src, {}, cnt, func);
		// bring data back to cpu
		auto res = download_buffer(calc).get<Buffer>(*test_context.allocator, test_context.compiler);
		auto out = std::span((uint32_t*)res->mapped_ptr, data.size());
		if (test_context.rdoc_api)
			test_context.rdoc_api->EndFrameCapture(NULL, NULL);
		CHECK(out == std::span(expected));
	}
	/* {
	  if (test_context.rdoc_api)
	    test_context.rdoc_api->StartFrameCapture(NULL, NULL);
	  // src data
	  std::vector data = { 1u, 2u, 3u };
	  // function to apply
	  auto func = [](auto A) {
	    return spirv::select(A > 1u, 1u, 2u);
	  };
	  std::vector<uint32_t> expected;
	  // cpu result
	  std::transform(data.begin(), data.end(), std::back_inserter(expected), func);

	  // put data on gpu
	  auto [_1, src] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(data));
	  // put count on gpu
	  CountWithIndirect count_data{ (uint32_t)data.size(), 64 };
	  auto [_2, cnt] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(&count_data, 1));

	  // apply function on gpu
	  auto calc = unary_map<uint32_t>(src, {}, cnt, func);
	  // bring data back to cpu
	  auto res = download_buffer(calc).get<Buffer>(*test_context.allocator, test_context.compiler);
	  auto out = std::span((uint32_t*)res->mapped_ptr, data.size());
	  if (test_context.rdoc_api)
	    test_context.rdoc_api->EndFrameCapture(NULL, NULL);
	  CHECK(out == std::span(expected));
	}*/
	/*{
		if (test_context.rdoc_api)
			test_context.rdoc_api->StartFrameCapture(NULL, NULL);
		// src data
		std::vector data = { 1.f, 2.f, 3.f };
		// function to apply
		auto func = [](auto A) {
			return spirv::select(A > 1.f, 3.f * A, 4.f);
		};
		std::vector<float> expected;
		// cpu result
		std::transform(data.begin(), data.end(), std::back_inserter(expected), func);

		// put data on gpu
		auto [_1, src] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(data));
		// put count on gpu
		CountWithIndirect count_data{ (uint32_t)data.size(), 64 };
		auto [_2, cnt] = create_buffer_gpu(*test_context.allocator, DomainFlagBits::eAny, std::span(&count_data, 1));

		// apply function on gpu
		auto calc = unary_map<float>(src, {}, cnt, func);
		// bring data back to cpu
		auto res = download_buffer(calc).get<Buffer>(*test_context.allocator, test_context.compiler);
		auto out = std::span((float*)res->mapped_ptr, data.size());
		if (test_context.rdoc_api)
			test_context.rdoc_api->EndFrameCapture(NULL, NULL);
		CHECK(out == std::span(expected));
	}*/
}