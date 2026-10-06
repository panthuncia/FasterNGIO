#include "Grass/NgioCacheWriter.h"

#include <bit>

namespace FasterNGIO::Grass
{
	namespace
	{
		void AppendU32(std::vector<std::uint8_t>& a_output, std::uint32_t a_value)
		{
			a_output.push_back(static_cast<std::uint8_t>(a_value & 0xFFu));
			a_output.push_back(static_cast<std::uint8_t>((a_value >> 8) & 0xFFu));
			a_output.push_back(static_cast<std::uint8_t>((a_value >> 16) & 0xFFu));
			a_output.push_back(static_cast<std::uint8_t>((a_value >> 24) & 0xFFu));
		}

		[[nodiscard]] std::size_t EncodedSize(const NgioCellCache& a_cache)
		{
			std::size_t size = 4;
			for (const auto& group : a_cache.groups) {
				size += 4 + group.modelPath.size() + 1 + 4 + 4 + 3 + 4;
				for (const auto& block : group.blocks) {
					size += block.descriptorWords.size() * 4 + block.payloadWords.size() * 2;
				}
			}
			return size;
		}
	}

	std::vector<std::uint8_t> SerializeNgioCellCache(const NgioCellCache& a_cache)
	{
		std::vector<std::uint8_t> output;
		output.reserve(EncodedSize(a_cache));
		AppendU32(output, static_cast<std::uint32_t>(a_cache.groups.size()));
		for (const auto& group : a_cache.groups) {
			AppendU32(output, static_cast<std::uint32_t>(group.modelPath.size() + 1));
			output.insert(output.end(), group.modelPath.begin(), group.modelPath.end());
			output.push_back(0);

			AppendU32(output, std::bit_cast<std::uint32_t>(group.wavePeriod));
			AppendU32(output, group.grassFormID);
			// Engine order (in-game caches): fit to slope, uniform scaling, vertex lighting. Vanilla GRAS
			// flags 0x06 are written 01 01 00; blades of types with the first byte set are tilted.
			output.push_back(group.fitToSlope ? 1 : 0);
			output.push_back(group.uniformScaling ? 1 : 0);
			output.push_back(group.vertexLighting ? 1 : 0);
			AppendU32(output, static_cast<std::uint32_t>(group.blocks.size()));

			for (const auto& block : group.blocks) {
				for (const auto word : block.descriptorWords) {
					AppendU32(output, word);
				}
				for (const auto word : block.payloadWords) {
					output.push_back(static_cast<std::uint8_t>(word & 0xFFu));
					output.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFFu));
				}
			}
		}
		return output;
	}
}
