#include "Grass/NgioCacheWriter.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	using namespace FasterNGIO;

	std::uint32_t U32At(const std::vector<std::uint8_t>& a_bytes, std::size_t a_offset)
	{
		return static_cast<std::uint32_t>(a_bytes[a_offset]) | (static_cast<std::uint32_t>(a_bytes[a_offset + 1]) << 8) |
		       (static_cast<std::uint32_t>(a_bytes[a_offset + 2]) << 16) | (static_cast<std::uint32_t>(a_bytes[a_offset + 3]) << 24);
	}
}

TEST(NgioCacheWriter, EmptyCellIsOneZeroWord)
{
	// The engine (and NGIO) write a file even for a cell without grass.
	const auto bytes = Grass::SerializeNgioCellCache({});
	ASSERT_EQ(bytes.size(), 4u);
	EXPECT_EQ(U32At(bytes, 0), 0u);
}

TEST(NgioCacheWriter, SerializesGroupsLittleEndian)
{
	Grass::NgioCellCache cache;
	auto& group = cache.groups.emplace_back();
	group.modelPath = "meshes\\grass\\a.nif";
	group.wavePeriod = 10.0f;
	group.grassFormID = 0x12345678;
	group.fitToSlope = true;
	auto& block = group.blocks.emplace_back();
	block.descriptorWords.fill(0xA1B2C3D4u);
	block.payloadWords = { 0x0102, 0xFFEE };

	const auto bytes = Grass::SerializeNgioCellCache(cache);
	// Group count; path length, path and terminator; wave period and form ID; three flags; block
	// count; nine descriptor words; the payload.
	ASSERT_EQ(bytes.size(), 4u + 4u + group.modelPath.size() + 1u + 4u + 4u + 3u + 4u + 9u * 4u + 2u * 2u);
	EXPECT_EQ(U32At(bytes, 0), 1u);
	const auto afterPath = 8u + group.modelPath.size() + 1u;
	EXPECT_EQ(U32At(bytes, afterPath), 0x41200000u);
	EXPECT_EQ(U32At(bytes, afterPath + 4), 0x12345678u);
	// engine order: fit to slope, uniform scaling, vertex lighting
	EXPECT_EQ(bytes[afterPath + 8], 1u);
	EXPECT_EQ(bytes[afterPath + 10], 0u);
	EXPECT_EQ(bytes[bytes.size() - 4], 0x02u);
	EXPECT_EQ(bytes[bytes.size() - 3], 0x01u);
	EXPECT_EQ(bytes[bytes.size() - 2], 0xEEu);
	EXPECT_EQ(bytes[bytes.size() - 1], 0xFFu);
}

