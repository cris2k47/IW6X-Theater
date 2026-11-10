#include <std_include.hpp>
#include "memory.hpp"
#include "compression.hpp"

#include <zlib.h>



namespace utils::compression
{
	namespace zlib
	{
		namespace
		{
			class zlib_stream
			{
			public:
				zlib_stream()
				{
					memset(&stream_, 0, sizeof(stream_));
					valid_ = inflateInit(&stream_) == Z_OK;
				}

				zlib_stream(zlib_stream&&) = delete;
				zlib_stream(const zlib_stream&) = delete;
				zlib_stream& operator=(zlib_stream&&) = delete;
				zlib_stream& operator=(const zlib_stream&) = delete;

				~zlib_stream()
				{
					if (valid_)
					{
						inflateEnd(&stream_);
					}
				}

				z_stream& get()
				{
					return stream_; //
				}

				bool is_valid() const
				{
					return valid_;
				}

			private:
				bool valid_{false};
				z_stream stream_{};
			};
		}

		std::string decompress(const std::string& data)
		{
			std::string buffer{};
			zlib_stream stream_container{};
			if (!stream_container.is_valid())
			{
				return {};
			}

			int ret{};
			size_t offset = 0;
			static thread_local uint8_t dest[CHUNK] = {0};
			auto& stream = stream_container.get();

			do
			{
				const auto input_size = std::min(sizeof(dest), data.size() - offset);
				stream.avail_in = static_cast<uInt>(input_size);
				stream.next_in = reinterpret_cast<const Bytef*>(data.data()) + offset;
				offset += stream.avail_in;

				do
				{
					stream.avail_out = sizeof(dest);
					stream.next_out = dest;

					ret = inflate(&stream, Z_NO_FLUSH);
					if (ret != Z_OK && ret != Z_STREAM_END)
					{
						return {};
					}

					buffer.insert(buffer.end(), dest, dest + sizeof(dest) - stream.avail_out);
				}
				while (stream.avail_out == 0);
			}
			while (ret != Z_STREAM_END);

			return buffer;
		}

		std::string compress(const std::string& data)
		{
			std::string result{};
			auto length = compressBound(static_cast<uLong>(data.size()));
			result.resize(length);

			if (compress2(reinterpret_cast<Bytef*>(result.data()), &length,
			              reinterpret_cast<const Bytef*>(data.data()), static_cast<uLong>(data.size()),
			              Z_BEST_COMPRESSION) != Z_OK)
			{
				return {};
			}

			result.resize(length);
			return result;
		}

		std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input, std::size_t exp_output_size, double growth_rate)
		{
			auto decompress_internal = [](std::uint8_t* output, uLong* output_size, const std::uint8_t* input, uLong* input_size)
			{
				return uncompress2(
					reinterpret_cast<Bytef*>(output), output_size,
					reinterpret_cast<const Bytef*>(input), input_size
				);
			};

			auto output_size = static_cast<uLong>(std::max(exp_output_size, std::size_t(256)));
			std::vector<std::uint8_t> output(output_size);

			auto input_size = static_cast<uLong>(input.size());
			auto status = decompress_internal(output.data(), &output_size, input.data(), &input_size);

			if (status == Z_BUF_ERROR)
			{
				const auto decompression_ratio = (input_size / static_cast<double>(output.size())) - 0.04;
				output_size = static_cast<uLong>(std::max(input.size() / decompression_ratio, static_cast<double>(output_size)));
				output.resize(output_size);

				input_size = static_cast<uLong>(input.size());
				status = decompress_internal(output.data(), &output_size, input.data(), &input_size);
			}

			for (growth_rate = std::max(growth_rate, 1.0); status == Z_BUF_ERROR;)
			{
				output_size = 4 + static_cast<uLong>(growth_rate * output.size());
				output.resize(output_size);

				input_size = static_cast<uLong>(input.size());
				status = decompress_internal(output.data(), &output_size, input.data(), &input_size);
			}

			if (status != Z_OK)
			{
				output.clear();
				return output;
			}

			output.resize(output_size);
			return output;
		}

		std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input, std::size_t exp_output_size, double growth_rate)
		{
			auto compress_internal = [](std::uint8_t* output, uLong* output_size, const std::uint8_t* input, uLong input_size)
			{
				return compress2(
					reinterpret_cast<Bytef*>(output), output_size,
					reinterpret_cast<const Bytef*>(input), input_size,
					Z_BEST_COMPRESSION
				);
			};

			auto output_size = static_cast<uLong>(std::max(exp_output_size, std::size_t(256)));
			std::vector<std::uint8_t> output(output_size);

			const auto input_size = static_cast<uLong>(input.size());
			auto status = compress_internal(output.data(), &output_size, input.data(), input_size);

			for (growth_rate = std::max(growth_rate, 1.0); status == Z_BUF_ERROR;)
			{
				output_size = 4 + static_cast<uLong>(growth_rate * output.size());
				output.resize(output_size);

				status = compress_internal(output.data(), &output_size, input.data(), input_size);
			}

			if (status != Z_OK)
			{
				output.clear();
				return output;
			}

			output.resize(output_size);
			return output;
		}
	}

}
