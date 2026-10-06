#pragma once

#include "Archives/ArchiveResolver.h"
#include "Platform/GameInstall.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace FasterNGIO::Grass
{
	struct PlacementSettings;

	// The folder the game reads Skyrim.ini, SkyrimCustom.ini and SkyrimPrefs.ini from.
	struct GameIniDirectory
	{
		std::filesystem::path path;
		// How it was found, for the log: "--game-ini-dir", "MO2 profile" or "My Games".
		std::string origin;
	};

	// In order: a_explicit; the MO2 profile holding a_pluginsTxt when that profile uses its own game
	// INIs (settings.ini [General] LocalSettings=true); the My Games folder of the install a_data
	// belongs to, named for its store (Documents on Windows, which MO2's virtual filesystem also
	// redirects to profile INIs; the Proton prefix elsewhere; a_userFolders overrides both). Null
	// when none exists.
	[[nodiscard]] std::optional<GameIniDirectory> LocateGameIniDirectory(
		const std::filesystem::path& a_pluginsTxt,
		const std::filesystem::path& a_data,
		const std::optional<std::filesystem::path>& a_explicit,
		const std::optional<Platform::UserFolders>& a_userFolders = std::nullopt);

	template <class T>
	struct GameIniValue
	{
		T value{};
		std::filesystem::path source;
	};

	// The [Grass] settings that change placement, as the game would end up with them.
	struct GrassIniSettings
	{
		std::optional<GameIniValue<std::uint32_t>> minGrassSize;             // iMinGrassSize
		std::optional<GameIniValue<std::uint32_t>> maxGrassTypesPerTexture;  // iMaxGrassTypesPerTexure (sic)
		std::optional<GameIniValue<float>> texturePctThreshold;              // fTexturePctThreshold
		std::vector<std::filesystem::path> filesRead;
	};

	// Reads Skyrim.ini and then SkyrimCustom.ini from a_directory (names matched case-insensitively),
	// the later file overriding, as the engine's Skyrim.ini setting collection does; then a_pluginInis,
	// the INIs the game loads next to each plugin (Data\<plugin name>.ini), in load order, each one
	// overriding. Grass mods set [Grass] there: Seasonal Landscapes - Unfrozen.ini's iMinGrassSize=30 is
	// what the game used over SkyrimCustom.ini's 60 (NGIO's in-game cache matches 30, not 60).
	[[nodiscard]] GrassIniSettings ReadGrassIniSettings(const std::filesystem::path& a_directory,
		const std::vector<std::filesystem::path>& a_pluginInis = {});

	void ApplyGrassIniSettings(const GrassIniSettings& a_ini, PlacementSettings& a_settings);

	// [Archive] sResourceArchiveList and sResourceArchiveList2 from the same files, in the same order:
	// the archives the game loads before the plugins' own, which grass models may come from.
	[[nodiscard]] Archives::ArchiveIniLists ReadArchiveIniLists(const std::filesystem::path& a_directory);
}
