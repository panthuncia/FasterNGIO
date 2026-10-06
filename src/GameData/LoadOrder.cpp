#include "GameData/LoadOrder.h"

#include "GameData/Internal/RecordReader.h"
#include "Platform/DataDirectory.h"
#include "Platform/Text.h"

#include <spdlog/spdlog.h>

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace FasterNGIO::GameData
{
	namespace
	{
		[[nodiscard]] bool IsPluginName(std::string_view a_name)
		{
			const auto lower = Platform::LowerAscii(a_name);
			return lower.ends_with(".esm") || lower.ends_with(".esp") || lower.ends_with(".esl");
		}
	}

	std::vector<LoadOrderEntry> ReadPluginsTxt(const std::filesystem::path& a_dataPath, const std::filesystem::path& a_pluginsTxtPath)
	{
		std::ifstream input(a_pluginsTxtPath);
		if (!input) {
			throw std::runtime_error("failed to open plugins.txt: " + a_pluginsTxtPath.string());
		}

		std::vector<LoadOrderEntry> result;
		std::unordered_set<std::string> added;
		const auto appendIfPresent = [&](std::string_view a_pluginName) {
			auto key = Platform::LowerAscii(a_pluginName);
			if (!IsPluginName(key) || added.contains(key)) {
				return;
			}
			// Plugin names are matched case-insensitively, as the game (and Proton) does.
			const auto path = Platform::FindInDirectory(a_dataPath, key);
			if (!path) {
				return;
			}
			result.push_back(LoadOrderEntry{ .path = *path, .pluginName = key });
			added.insert(std::move(key));
		};

		for (const auto* master : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
			appendIfPresent(master);
		}

		if (const auto cccPath = Platform::FindInDirectory(a_dataPath.parent_path(), "Skyrim.ccc")) {
			std::ifstream cccInput(*cccPath);
			for (std::string line; std::getline(cccInput, line);) {
				const auto trimmed = Platform::Trim(line);
				if (!trimmed.empty() && !trimmed.starts_with('#')) {
					appendIfPresent(trimmed);
				}
			}
		}

		for (std::string line; std::getline(input, line);) {
			const auto trimmed = Platform::Trim(line);
			// Skyrim SE/AE and MO2 prefix enabled entries with '*'. Unstarred entries remain in
			// plugins.txt to preserve their ordering, but are disabled and must not contribute
			// records to the resolved load order.
			if (!trimmed.starts_with('*')) {
				continue;
			}
			appendIfPresent(Platform::Trim(trimmed.substr(1)));
		}
		return result;
	}

	std::vector<LoadOrderEntry> PrepareLoadOrder(std::vector<LoadOrderEntry> a_entries)
	{
		oneapi::tbb::parallel_for(std::size_t{ 0 }, a_entries.size(), [&](std::size_t i) {
			auto& entry = a_entries[i];
			entry.pluginName = entry.pluginName.empty() ? Platform::LowerAscii(entry.path.filename().string()) : Platform::LowerAscii(entry.pluginName);
			const auto header = Internal::ReadPluginHeader(entry.path);
			entry.recordCount = header.recordCount;
			const bool light = Platform::LowerAscii(entry.path.extension().string()) == ".esl" || (header.flags & Internal::kTes4FlagLight) != 0;
			entry.kind = light ? ModuleKind::Light : ModuleKind::Full;
			entry.masters.clear();
			for (const auto& master : header.masters) {
				entry.masters.push_back(Platform::LowerAscii(master));
			}
		});

		// Past the engine's 254 full (00-FD) or 4096 light slots a plugin cannot load; drop it, and then
		// every plugin that has a dropped or absent master, and report them, instead of refusing the
		// whole load order (a 4,000-plugin list sat at 255 full after CC masters lost their ESL flag).
		std::size_t full = 0;
		std::size_t light = 0;
		std::unordered_set<std::string> present;
		std::vector<LoadOrderEntry> kept;
		kept.reserve(a_entries.size());
		for (auto& entry : a_entries) {
			const bool isLight = entry.kind == ModuleKind::Light;
			if (isLight ? light >= 0x1000u : full >= 0xFEu) {
				spdlog::error("load order: no {} slot left for {}; it is not loaded (as in game)", isLight ? "light" : "full", entry.pluginName);
				continue;
			}
			if (const auto missing = std::ranges::find_if(entry.masters, [&](const std::string& a_master) { return !present.contains(a_master); });
				missing != entry.masters.end()) {
				spdlog::error("load order: {} needs {}, which is not loaded; it is not loaded either", entry.pluginName, *missing);
				continue;
			}
			(isLight ? light : full)++;
			present.insert(entry.pluginName);
			kept.push_back(std::move(entry));
		}
		std::uint16_t fullSlot = 0;
		std::uint16_t lightSlot = 0;
		for (auto& entry : kept) {
			entry.fileID = entry.kind == ModuleKind::Light ? FileID{ .kind = ModuleKind::Light, .slot = lightSlot++ } :
			                                                  FileID{ .kind = ModuleKind::Full, .slot = fullSlot++ };
		}
		return kept;
	}
}
