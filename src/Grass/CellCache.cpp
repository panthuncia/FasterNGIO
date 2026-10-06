#include "Grass/CellCache.h"

#include "Grass/Internal/PlacementCommon.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace FasterNGIO::Grass
{
	namespace
	{
		constexpr std::size_t kQuadrants = GameData::LandInfo::QuadrantCount;

		// Keeps a_keep of a_blades, evenly spaced through placement order (so through the quadrant's
		// patches): blade k stays when floor((k + 1) * keep / n) steps past floor(k * keep / n).
		void ThinEvenly(std::vector<std::uint32_t>& a_blades, std::uint32_t a_keep)
		{
			const auto count = static_cast<std::uint64_t>(a_blades.size());
			std::size_t kept = 0;
			for (std::uint64_t k = 0; k < count; ++k) {
				if ((k + 1) * a_keep / count != k * a_keep / count) {
					a_blades[kept++] = a_blades[static_cast<std::size_t>(k)];
				}
			}
			a_blades.resize(kept);
		}

		// One block, with the descriptor BSMultiStreamInstanceTriShape::AddGroup computes: the bounds of
		// the stored half-float positions (x and y offset by the cache block's corner, the shape's
		// origin), padded by 30 units, as center and half extents; then the triangle count, the blade
		// count and the words per blade. The engine's triangle count is the shape's meshTriCount, which
		// its constructor never sets, times the blade count; it is only stored, so 0 is written.
		// Comparisons and arithmetic follow the engine's SSE code, so the floats come out identical.
		NgioGrassGeometryBlock MakeBlock(const CellCandidates& a_candidates, std::span<const std::uint32_t> a_blades)
		{
			constexpr float kPad = 30.0f;
			const auto baseX = Internal::BlockBase(a_candidates.cellX);
			const auto baseY = Internal::BlockBase(a_candidates.cellY);
			float lo[3]{ (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
			float hi[3]{ -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };
			NgioGrassGeometryBlock block;
			block.payloadWords.reserve(a_blades.size() * kBladeWords);
			for (const auto index : a_blades) {
				const auto& words = a_candidates.blades[index].words;
				block.payloadWords.insert(block.payloadWords.end(), words.begin(), words.end());
				const auto x = Internal::HalfBitsToFloat(words[0]) + baseX;
				const auto y = Internal::HalfBitsToFloat(words[1]) + baseY;
				const auto z = Internal::HalfBitsToFloat(words[2]);
				// minss/maxss keep the running value on ties; the z tests (comiss) take the new one.
				lo[0] = x < lo[0] ? x : lo[0];
				hi[0] = x > hi[0] ? x : hi[0];
				lo[1] = y < lo[1] ? y : lo[1];
				hi[1] = y > hi[1] ? y : hi[1];
				lo[2] = lo[2] < z ? lo[2] : z;
				hi[2] = hi[2] > z ? hi[2] : z;
			}
			std::array<float, 3> center{};
			std::array<float, 3> extent{};
			for (std::size_t axis = 0; axis < 3; ++axis) {
				const float min = lo[axis] - kPad;
				const float max = hi[axis] + kPad;
				const float half = (max - min) * 0.5f;
				center[axis] = min + half;
				extent[axis] = max - center[axis];
			}
			block.descriptorWords = {
				std::bit_cast<std::uint32_t>(center[0]),
				std::bit_cast<std::uint32_t>(center[1]),
				std::bit_cast<std::uint32_t>(center[2]),
				std::bit_cast<std::uint32_t>(extent[0]),
				std::bit_cast<std::uint32_t>(extent[1]),
				std::bit_cast<std::uint32_t>(extent[2]),
				0u,
				static_cast<std::uint32_t>(a_blades.size()),
				kBladeWords,
			};
			return block;
		}
	}

	std::uint32_t BladesPerBlock(std::uint32_t a_triangles, std::uint32_t a_vertices)
	{
		// The engine reads both counts as 16-bit fields of the shape.
		const auto triangles = a_triangles & 0xFFFFu;
		const auto vertices = a_vertices & 0xFFFFu;
		if (triangles == 0 || vertices == 0) {
			return kMaxBladesPerBlock;
		}
		const auto blades = (std::min)(0xFFFFu / (3u * triangles), 0xFFFFu / vertices);
		return std::clamp(blades, 1u, kMaxBladesPerBlock);
	}

	FinalizedCell FinalizeCell(const CellCandidates& a_candidates, std::span<const std::uint32_t> a_rejected, const BlockLayout& a_layout)
	{
		// The surviving blades of each group and quadrant, in placement order.
		std::vector<std::vector<std::uint32_t>> kept(a_candidates.groups.size() * kQuadrants);
		for (std::size_t i = 0; i < a_candidates.blades.size(); ++i) {
			if (!a_rejected.empty() && (a_rejected[i / 32] & (1u << (i % 32))) != 0) {
				continue;
			}
			const auto& blade = a_candidates.blades[i];
			kept[blade.groupIndex * kQuadrants + blade.quadrant].push_back(static_cast<std::uint32_t>(i));
		}

		FinalizedCell result;
		if (a_layout.capQuadrantBlades) {
			for (auto& blades : kept) {
				if (blades.size() > kMaxBladesPerQuadrant) {
					result.bladesCapped += static_cast<std::uint32_t>(blades.size() - kMaxBladesPerQuadrant);
					ThinEvenly(blades, kMaxBladesPerQuadrant);
				}
			}
		}

		std::vector<std::uint32_t> order;
		for (std::uint32_t i = 0; i < a_candidates.groups.size(); ++i) {
			const auto first = kept.begin() + static_cast<std::ptrdiff_t>(i * kQuadrants);
			if (std::any_of(first, first + kQuadrants, [](const std::vector<std::uint32_t>& a_blades) { return !a_blades.empty(); })) {
				order.push_back(i);
			}
		}
		std::ranges::sort(order, [&](std::uint32_t lhs, std::uint32_t rhs) {
			return a_candidates.groups[lhs].grass->formID < a_candidates.groups[rhs].grass->formID;
		});

		for (const auto index : order) {
			const auto& source = a_candidates.groups[index];
			NgioGrassGroup group;
			// The engine writes the GRAS record's own MODL string ("landscape\Grass\DeadPineDrJ03.nif",
			// case kept, no meshes folder prefix), not the archive key placement uses (source.modelPath).
			group.modelPath = source.grass->modelPath;
			while (!group.modelPath.empty() && (group.modelPath.back() == '\0' || group.modelPath.back() == ' ')) {
				group.modelPath.pop_back();
			}
			group.grassFormID = source.grass->formID.value;
			group.wavePeriod = source.grass->wavePeriod;
			group.vertexLighting = source.grass->HasVertexLighting();
			group.uniformScaling = source.grass->HasUniformScaling();
			group.fitToSlope = source.grass->FitsToSlope();
			auto perBlock = kMaxBladesPerBlock;
			if (a_layout.bladesPerBlock) {
				if (const auto it = a_layout.bladesPerBlock->find(source.grass->formID); it != a_layout.bladesPerBlock->end()) {
					perBlock = it->second;
				}
			}
			for (std::size_t quadrant = 0; quadrant < kQuadrants; ++quadrant) {
				const std::span<const std::uint32_t> blades = kept[index * kQuadrants + quadrant];
				for (std::size_t start = 0; start < blades.size(); start += perBlock) {
					group.blocks.push_back(MakeBlock(a_candidates, blades.subspan(start, (std::min)(blades.size() - start, static_cast<std::size_t>(perBlock)))));
				}
			}
			result.cache.groups.push_back(std::move(group));
		}
		return result;
	}

	std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		return std::format("{}x{:04}y{:04}.cgid", a_worldEditorID, a_cellX, a_cellY);
	}

	std::string MakeNgioCacheFileName(std::string_view a_worldEditorID, std::int32_t a_cellX, std::int32_t a_cellY, std::string_view a_season)
	{
		if (a_season.empty()) {
			return MakeNgioCacheFileName(a_worldEditorID, a_cellX, a_cellY);
		}
		return std::format("{}x{:04}y{:04}.{}.cgid", a_worldEditorID, a_cellX, a_cellY, a_season);
	}

	std::string ResolveWorldEditorID(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID)
	{
		const auto worldIt = a_snapshot.worldsByFormID.find(a_worldFormID);
		if (worldIt != a_snapshot.worldsByFormID.end() && !worldIt->second.editorID.empty()) {
			return worldIt->second.editorID;
		}
		if (a_worldFormID.value == 0x0000003Cu) {
			return "Tamriel";
		}
		return std::format("{:08X}", a_worldFormID.value);
	}

	bool ExistingNgioCacheLooksValid(const std::filesystem::path& a_path)
	{
		// More groups than this is not a cache NGIO wrote.
		constexpr std::uint32_t kMaxPlausibleGroups = 4096;
		std::error_code ec;
		const auto size = std::filesystem::file_size(a_path, ec);
		if (ec || size < sizeof(std::uint32_t)) {
			return false;
		}
		std::ifstream input(a_path, std::ios::binary);
		std::array<unsigned char, 4> bytes{};
		input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!input) {
			return false;
		}
		const auto groupCount = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8u) |
		                        (static_cast<std::uint32_t>(bytes[2]) << 16u) | (static_cast<std::uint32_t>(bytes[3]) << 24u);
		return groupCount <= kMaxPlausibleGroups;
	}
}
