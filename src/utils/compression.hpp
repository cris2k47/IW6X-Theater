#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#define CHUNK 16384u

namespace utils::compression
{
	namespace zlib
	{
		std::string compress(const std::string& data);
		std::string decompress(const std::string& data);

		std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input, std::size_t exp_output_size, double growth_rate = 2.0);
		std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input, std::size_t exp_output_size, double growth_rate = 2.0);
	}

};
