#include "WorldSetup.h"

#include "Grass/CellCache.h"
#include "GameData/LoadOrder.h"
#include "Grass/GameIni.h"
#include "Platform/DataDirectory.h"
#include "Platform/FileSystem.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <unordered_set>

namespace FasterNGIO::App
{
	bool IsSelectedCell(const GenerateOptions& a_options, std::int32_t a_x, std::int32_t a_y)
	{
		if (a_options.singleCellX && a_options.singleCellY) {
			return a_x == *a_options.singleCellX && a_y == *a_options.singleCellY;
		}
		if (a_options.centerCellX && a_options.centerCellY && a_options.radius) {
			return std::abs(a_x - *a_options.centerCellX) <= *a_options.radius && std::abs(a_y - *a_options.centerCellY) <= *a_options.radius;
		}
		return true;
	}

	double SecondsSince(std::chrono::steady_clock::time_point a_begin)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - a_begin).count();
	}

	QueryShapes MakeQueryShapes(const GameData::StaticWorldSnapshot& a_snapshot, const Rejection::RejectionConfig& a_config)
	{
		QueryShapes shapes;
		for (const auto& [formID, grass] : a_snapshot.grassesByFormID) {
			const auto shape = Rejection::MakeQueryShape(a_config, grass);
			shapes.byGrass.emplace(formID, shape);
			shapes.maxReach = (std::max)(shapes.maxReach, shape.radius);
		}
		return shapes;
	}

	Grass::PlacementSettings ResolvePlacementSettings(const GenerateOptions& a_options)
	{
		auto settings = a_options.placement;
		if (a_options.readGameIni) {
			if (const auto directory = Grass::LocateGameIniDirectory(a_options.pluginsTxtPath, a_options.dataPath, a_options.gameIniDirectory)) {
				// The INIs the game loads beside each active plugin, in load order.
				std::vector<std::filesystem::path> pluginInis;
				for (const auto& entry : GameData::ReadPluginsTxt(a_options.dataPath, a_options.pluginsTxtPath)) {
					if (const auto ini = Platform::FindInDirectory(a_options.dataPath, entry.path.stem().string() + ".ini")) {
						pluginInis.push_back(*ini);
					}
				}
				const auto ini = Grass::ReadGrassIniSettings(directory->path, pluginInis);
				for (const auto& file : ini.filesRead) {
					if (std::ranges::find(pluginInis, file) != pluginInis.end()) {
						spdlog::info("game INI: plugin INI {}", file.filename().string());
					}
				}
				Grass::ApplyGrassIniSettings(ini, settings);
				if (ini.filesRead.empty()) {
					spdlog::info("game INI: no Skyrim.ini in {} ({}); using engine defaults", directory->path.string(), directory->origin);
				} else {
					spdlog::info("game INI: {} ({})", directory->path.string(), directory->origin);
				}
				const auto describe = [](const auto& a_value) { return a_value ? std::format("{} ({})", a_value->value, a_value->source.filename().string()) : std::string("engine default"); };
				spdlog::info("game INI [Grass]: iMinGrassSize={} iMaxGrassTypesPerTexure={} fTexturePctThreshold={}", describe(ini.minGrassSize),
					describe(ini.maxGrassTypesPerTexture), describe(ini.texturePctThreshold));
			} else if (a_options.gameIniDirectory) {
				throw std::invalid_argument("the game INI folder is not a folder: " + a_options.gameIniDirectory->string());
			} else {
				spdlog::info("game INI: none found; using engine defaults");
			}
		}
		// NGIO's Ensure-max-grass-types-setting raises the INI's value; the command line still wins.
		if (a_options.ensureMaxGrassTypes) {
			settings.maxGrassTypesPerTexture = (std::max)(settings.maxGrassTypesPerTexture, *a_options.ensureMaxGrassTypes);
		}
		if (a_options.maxGrassTypesOverride) {
			settings.maxGrassTypesPerTexture = *a_options.maxGrassTypesOverride;
		}
		if (a_options.minGrassSizeOverride) {
			settings.minGrassSize = *a_options.minGrassSizeOverride;
		}
		if (a_options.alphaThresholdOverride) {
			settings.alphaThreshold = *a_options.alphaThresholdOverride;
		}
		spdlog::info("placement: {}, iMinGrassSize={} iMaxGrassTypesPerTexure={} fTexturePctThreshold={}", PlacementName(settings.mode), settings.minGrassSize,
			settings.maxGrassTypesPerTexture, settings.alphaThreshold);
		return settings;
	}

	std::vector<const GameData::LandInfo*> SelectLands(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID, const GenerateOptions& a_options)
	{
		const auto landsIt = a_snapshot.landsByWorldspace.find(a_worldFormID);
		if (landsIt == a_snapshot.landsByWorldspace.end() || landsIt->second.empty()) {
			throw std::runtime_error(std::format("no LAND records were loaded for world {:08X}", a_worldFormID.value));
		}
		std::vector<const GameData::LandInfo*> lands;
		std::unordered_set<std::uint64_t> seenCells;
		for (const auto& land : landsIt->second) {
			if (land.cellX && land.cellY && IsSelectedCell(a_options, *land.cellX, *land.cellY) && seenCells.insert(GameData::PackCellCoords(*land.cellX, *land.cellY)).second) {
				lands.push_back(std::addressof(land));
			}
		}
		std::ranges::sort(lands, [](const GameData::LandInfo* lhs, const GameData::LandInfo* rhs) {
			return *lhs->cellY == *rhs->cellY ? *lhs->cellX < *rhs->cellX : *lhs->cellY < *rhs->cellY;
		});
		return lands;
	}

	WorldPlacement PrepareWorldPlacement(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID, const Grass::PlacementSettings& a_settings)
	{
		WorldPlacement placement;
		placement.settings = a_settings;
		if (a_settings.mode != Grass::PlacementMode::Smooth) {
			return placement;
		}
		// The field covers the whole worldspace whatever cells are selected, so a cell comes out the
		// same however a run selects it.
		const auto begin = std::chrono::steady_clock::now();
		placement.field = std::make_unique<Grass::SmoothWeightField>(a_snapshot, a_snapshot.landsByWorldspace.at(a_worldFormID), a_settings);
		placement.settings.smooth.field = placement.field.get();
		const auto& field = *placement.field;
		spdlog::info("smooth placement: weight grids for {} grass type(s) over {} cell(s) built in {:.3f}s", field.GridCount(), field.CellCount(), SecondsSince(begin));
		if (a_settings.smooth.matchVanillaDensity) {
			double vanilla = 0.0;
			double smooth = 0.0;
			for (const auto& [grass, expected] : field.Expected()) {
				vanilla += expected.vanilla;
				smooth += expected.smooth;
				spdlog::debug("density match {:08X}: vanilla {:.0f} smooth {:.0f} scale {:.3f}", grass.value, expected.vanilla, expected.smooth, field.DensityScale(grass));
			}
			spdlog::info("smooth placement: per-type density scaled to vanilla's worldspace totals ({:.0f} expected blades vs {:.0f} unscaled)", vanilla, smooth);
		}
		return placement;
	}

	Archives::ArchiveResolver MakeResolver(const GenerateOptions& a_options, const LoadedPlugins& a_plugins)
	{
		Archives::ArchiveIniLists ini;
		if (a_options.readGameIni) {
			if (const auto directory = Grass::LocateGameIniDirectory(a_options.pluginsTxtPath, a_options.dataPath, a_options.gameIniDirectory)) {
				ini = Grass::ReadArchiveIniLists(directory->path);
				if (ini.resourceArchiveList || ini.resourceArchiveList2) {
					const auto describe = [](const auto& a_list) { return a_list ? std::format("{} archive(s)", a_list->size()) : std::string("default"); };
					spdlog::info("game INI [Archive]: sResourceArchiveList={} sResourceArchiveList2={}", describe(ini.resourceArchiveList), describe(ini.resourceArchiveList2));
				}
			}
		}
		return Archives::ArchiveResolver(a_options.dataPath, Archives::DefaultArchiveOrder(a_plugins.loadOrder, ini));
	}

	std::shared_ptr<const Rejection::WorldIndex> BuildWorldIndex(const GameData::StaticWorldSnapshot& a_snapshot, GameData::FormID a_worldFormID,
		const Archives::ArchiveResolver& a_resolver, const Rejection::RejectionFeatures& a_features, float a_maxReach)
	{
		auto index = std::make_shared<const Rejection::WorldIndex>(a_snapshot, a_worldFormID, a_resolver, a_features, a_maxReach);
		const auto& stats = index->Stats();
		spdlog::info("collision: {} model(s), {} with rejecting collision, {} missing ({:.2f}s); {} of {} reference(s) instanced{}", stats.models,
			stats.modelsWithCollision, stats.modelsMissing, stats.extractSeconds, stats.referencesWithCollision, stats.references,
			stats.referencesIgnored ? std::format(", {} ignored (Ray-cast-ignore-forms)", stats.referencesIgnored) : std::string{});
		if (stats.referencesSwapped != 0) {
			spdlog::info("collision: {} reference(s) use their season's replacement base object", stats.referencesSwapped);
		}
		if (a_features.renderGeometry) {
			spdlog::info("collision: {} of {} model(s) reject by their render geometry (experimental)", stats.modelsWithRenderGeometry,
				stats.modelsWithCollision);
		}
		if (stats.cliffInstances != 0 || stats.partIgnoredInstances != 0) {
			spdlog::info("collision: {} grass cliff instance(s), {} with ignored shapes", stats.cliffInstances, stats.partIgnoredInstances);
		}
		return index;
	}

#if FASTERNGIO_HAS_GPU
	std::unique_ptr<Gpu::GpuRejector> CreateGpuRejector(const GenerateOptions& a_options, const QueryShapes& a_shapes)
	{
		const auto exeDirectory = Platform::ExecutableDirectory();
		// Compiled shaders are cached per user: the program folder may be read-only (Program Files).
		const auto cacheRoot = Platform::CacheDirectory();
		return std::make_unique<Gpu::GpuRejector>(Gpu::GpuRejectorDesc{
			.api = a_options.gpuApi,
			.shaderDirectory = exeDirectory / "shaders",
			.shaderCacheDirectory = (cacheRoot.empty() ? exeDirectory : cacheRoot) / "shadercache",
			.mode = a_options.rejectionConfig.mode,
			.segmentLength = a_options.rejectionConfig.rayDepth + a_options.rejectionConfig.rayHeight,
			.maxQueryRadius = a_shapes.maxReach,
			.debugLayer = a_options.gpuDebugLayer,
		});
	}
#endif
}
