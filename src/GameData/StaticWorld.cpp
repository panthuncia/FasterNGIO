#include "GameData/StaticWorld.h"

#include "GameData/Internal/RecordReader.h"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace FasterNGIO::GameData
{
	namespace
	{
		using namespace Internal;

		// An open-addressing set of form IDs, for walking the load order backwards and keeping the
		// first (winning) version of each form.
		class FormIDSeenSet
		{
		public:
			explicit FormIDSeenSet(std::size_t a_expectedCount)
			{
				std::size_t capacity = 1;
				const auto target = (std::max<std::size_t>)(16, a_expectedCount * 2);
				while (capacity < target) {
					capacity <<= 1;
				}
				_slots.assign(capacity, 0);
				_mask = capacity - 1;
			}

			// False when a_formID was already in the set (or is null).
			[[nodiscard]] bool Insert(FormID a_formID)
			{
				const auto value = a_formID.value;
				if (value == 0) {
					return false;
				}
				for (auto index = Hash(value) & _mask;; index = (index + 1) & _mask) {
					auto& slot = _slots[index];
					if (slot == value) {
						return false;
					}
					if (slot == 0) {
						slot = value;
						return true;
					}
				}
			}

		private:
			[[nodiscard]] static std::size_t Hash(std::uint32_t a_value) noexcept
			{
				auto value = a_value;
				value ^= value >> 16;
				value *= 0x7feb352du;
				value ^= value >> 15;
				value *= 0x846ca68bu;
				value ^= value >> 16;
				return value;
			}

			std::vector<std::uint32_t> _slots;
			std::size_t _mask{ 0 };
		};

		// A reference's exterior cell: the parent cell's grid, or its position when the cell has
		// none. Nullopt in an interior cell.
		[[nodiscard]] std::optional<CellKey> ExteriorCellOf(const CellInfo& a_cell, const PlacementInfo& a_placement)
		{
			if (!a_cell.worldFormID || a_cell.IsInterior()) {
				return std::nullopt;
			}
			if (a_cell.gridX && a_cell.gridY) {
				return CellKey{ *a_cell.worldFormID, *a_cell.gridX, *a_cell.gridY };
			}
			const auto toCell = [](float a_coordinate) { return static_cast<std::int32_t>(std::floor(a_coordinate / kSkyrimTerrainCellSize)); };
			return CellKey{ *a_cell.worldFormID, toCell(a_placement.position[0]), toCell(a_placement.position[1]) };
		}

		// Later plugins overwrite earlier ones; a deleted or ignored record removes the form.
		template <class Info>
		void MergeRecords(std::unordered_map<FormID, Info, FormIDHash>& a_map, const std::vector<Info>& a_records)
		{
			for (const auto& record : a_records) {
				if (!record.formID.IsEmpty()) {
					a_map.insert_or_assign(record.formID, record);
				}
			}
		}

		// The first definition of each form of a_records: its creation order and editor ID.
		template <class Info>
		void RecordFirstDefinitions(StaticWorldSnapshot& a_snapshot, const std::vector<Info>& a_records)
		{
			for (const auto& record : a_records) {
				if (record.formID.IsEmpty()) {
					continue;
				}
				const auto order = static_cast<std::uint32_t>(a_snapshot.creationOrder.size());
				if (a_snapshot.creationOrder.try_emplace(record.formID, order).second && !record.editorID.empty()) {
					a_snapshot.firstEditorIDs.emplace(record.formID, record.editorID);
				}
			}
		}

		void MergeShards(StaticWorldSnapshot& a_snapshot, std::span<const StaticPluginShard> a_shards)
		{
			std::size_t bases = 0;
			std::size_t landTextures = 0;
			std::size_t grasses = 0;
			std::size_t worlds = 0;
			std::size_t cells = 0;
			for (const auto& shard : a_shards) {
				bases += shard.baseObjects.size();
				landTextures += shard.landTextures.size();
				grasses += shard.grasses.size();
				worlds += shard.worlds.size();
				cells += shard.cells.size();
			}
			a_snapshot.baseObjectsByFormID.reserve(bases);
			a_snapshot.landTexturesByFormID.reserve(landTextures);
			a_snapshot.grassesByFormID.reserve(grasses);
			a_snapshot.worldsByFormID.reserve(worlds);
			a_snapshot.cellsByFormID.reserve(cells);

			for (const auto& shard : a_shards) {
				// Forms are created in file order; a later plugin's override keeps the original's place.
				RecordFirstDefinitions(a_snapshot, shard.landTextures);
				RecordFirstDefinitions(a_snapshot, shard.materialTypes);
				RecordFirstDefinitions(a_snapshot, shard.textureSets);
				RecordFirstDefinitions(a_snapshot, shard.materialObjects);
				RecordFirstDefinitions(a_snapshot, shard.baseObjects);

				MergeRecords(a_snapshot.worldsByFormID, shard.worlds);
				MergeRecords(a_snapshot.baseObjectsByFormID, shard.baseObjects);
				MergeRecords(a_snapshot.landTexturesByFormID, shard.landTextures);
				MergeRecords(a_snapshot.grassesByFormID, shard.grasses);
				MergeRecords(a_snapshot.materialTypesByFormID, shard.materialTypes);
				MergeRecords(a_snapshot.textureSetsByFormID, shard.textureSets);
				MergeRecords(a_snapshot.materialObjectsByFormID, shard.materialObjects);
				MergeRecords(a_snapshot.cellsByFormID, shard.cells);
				for (const auto& suppressor : shard.suppressors) {
					if (IsBaseObjectSignature(suppressor.signature)) {
						a_snapshot.baseObjectsByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigLtex) {
						a_snapshot.landTexturesByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigGras) {
						a_snapshot.grassesByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigMatt) {
						a_snapshot.materialTypesByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigTxst) {
						a_snapshot.textureSetsByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigMato) {
						a_snapshot.materialObjectsByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigWrld) {
						a_snapshot.worldsByFormID.erase(suppressor.formID);
					} else if (suppressor.signature == kSigCell) {
						a_snapshot.cellsByFormID.erase(suppressor.formID);
					}
				}
			}
		}

		// The winning version of each form of a_signatures, walking the load order backwards: the
		// first version seen wins, and a suppressor seen first hides the form.
		template <class Info, class Visit>
		void ForEachWinner(std::span<const StaticPluginShard> a_shards, std::vector<Info> StaticPluginShard::*a_records, FourCC a_signature,
			FourCC a_otherSignature, Visit&& a_visit)
		{
			std::size_t count = 0;
			for (const auto& shard : a_shards) {
				count += (shard.*a_records).size() + shard.suppressors.size();
			}
			FormIDSeenSet seen{ count };
			for (auto shardIt = a_shards.rbegin(); shardIt != a_shards.rend(); ++shardIt) {
				for (const auto& suppressor : shardIt->suppressors) {
					if (suppressor.signature == a_signature || suppressor.signature == a_otherSignature) {
						(void)seen.Insert(suppressor.formID);
					}
				}
				const auto& records = (*shardIt).*a_records;
				for (auto it = records.rbegin(); it != records.rend(); ++it) {
					if (!it->formID.IsEmpty() && seen.Insert(it->formID)) {
						a_visit(*it);
					}
				}
			}
		}

		// The worldspace DNAM land height, from the parent while the child has "Use Land Data" (as the
		// water height in Placement.cpp; depth-capped against cycles).
		std::optional<float> DefaultLandHeight(const StaticWorldSnapshot& a_snapshot, FormID a_world)
		{
			for (int depth = 0; depth < 8; ++depth) {
				const auto it = a_snapshot.worldsByFormID.find(a_world);
				if (it == a_snapshot.worldsByFormID.end()) {
					return std::nullopt;
				}
				if (it->second.UsesParentLandData()) {
					a_world = *it->second.parentWorldFormID;
					continue;
				}
				return it->second.defaultLandHeight;
			}
			return std::nullopt;
		}

		void SelectLands(StaticWorldSnapshot& a_snapshot, std::span<const StaticPluginShard> a_shards)
		{
			// A LandInfo is ~8 KB of fixed arrays, so a move is a copy. Select the winners first, then
			// size each worldspace once and copy every winner exactly once.
			std::vector<std::pair<const LandInfo*, const CellInfo*>> winners;
			std::unordered_map<FormID, std::size_t, FormIDHash> countsByWorld;
			ForEachWinner(a_shards, &StaticPluginShard::lands, kSigLand, kSigLand, [&](const LandInfo& a_land) {
				const auto cellIt = a_snapshot.cellsByFormID.find(a_land.parentCell);
				if (cellIt == a_snapshot.cellsByFormID.end() || !cellIt->second.worldFormID || !cellIt->second.gridX || !cellIt->second.gridY) {
					return;
				}
				winners.emplace_back(std::addressof(a_land), std::addressof(cellIt->second));
				++countsByWorld[*cellIt->second.worldFormID];
			});
			for (const auto& [worldFormID, count] : countsByWorld) {
				a_snapshot.landsByWorldspace[worldFormID].reserve(count);
			}
			for (const auto& [source, cell] : winners) {
				auto& land = a_snapshot.landsByWorldspace[*cell->worldFormID].emplace_back(*source);
				land.worldFormID = cell->worldFormID;
				land.cellX = cell->gridX;
				land.cellY = cell->gridY;
				if (!land.hasHeights) {
					if (const auto height = DefaultLandHeight(a_snapshot, *cell->worldFormID)) {
						land.heights.fill(*height);
						land.hasHeights = true;
					}
				}
			}
		}

		void BucketPlacements(StaticWorldSnapshot& a_snapshot, std::span<const StaticPluginShard> a_shards)
		{
			std::vector<const PlacementInfo*> winners;
			ForEachWinner(a_shards, &StaticPluginShard::placements, kSigRefr, kSigAchr,
				[&](const PlacementInfo& a_placement) { winners.push_back(std::addressof(a_placement)); });

			// Enabled references of a known base object in an exterior cell.
			using Bucket = std::vector<std::pair<CellKey, PlacementInfo>>;
			oneapi::tbb::enumerable_thread_specific<Bucket> buckets;
			oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<std::size_t>(0, winners.size(), 4096), [&](const oneapi::tbb::blocked_range<std::size_t>& a_range) {
				auto& bucket = buckets.local();
				for (std::size_t i = a_range.begin(); i != a_range.end(); ++i) {
					PlacementInfo placement = *winners[i];
					if (placement.IsInitiallyDisabled() || !a_snapshot.baseObjectsByFormID.contains(placement.baseFormID)) {
						continue;
					}
					const auto cellIt = a_snapshot.cellsByFormID.find(placement.parentCell);
					if (cellIt == a_snapshot.cellsByFormID.end()) {
						continue;
					}
					placement.worldFormID = cellIt->second.worldFormID;
					if (const auto cell = ExteriorCellOf(cellIt->second, placement)) {
						bucket.emplace_back(*cell, std::move(placement));
					}
				}
			});

			a_snapshot.exteriorPlacementsByCell.reserve(16000);
			for (auto& bucket : buckets) {
				for (auto& [cell, placement] : bucket) {
					a_snapshot.exteriorPlacementsByCell[cell].push_back(std::move(placement));
				}
			}
		}
	}

	StaticWorldSnapshot BuildStaticWorldSnapshot(std::span<const StaticPluginShard> a_shards)
	{
		StaticWorldSnapshot snapshot;
		MergeShards(snapshot, a_shards);
		SelectLands(snapshot, a_shards);
		BucketPlacements(snapshot, a_shards);
		return snapshot;
	}
}
