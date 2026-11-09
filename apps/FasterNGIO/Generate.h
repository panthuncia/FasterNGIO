#pragma once

#include "GameData/GameData.h"
#include "Options.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Cache generation, shared by the command line (Main.cpp) and the launcher window (Gui/).
namespace FasterNGIO::App
{
	// A load order and its snapshot.
	struct LoadedPlugins
	{
		std::vector<GameData::LoadOrderEntry> loadOrder;
		GameData::StaticWorldSnapshot snapshot;
	};

	enum class RunStage : std::uint32_t
	{
		LoadingPlugins,
		// Only with Seasons of Skyrim on: working out its automatic winter swaps and reading its swap files.
		WorkingOutSeasons,
		// Reading the game's archive list and opening the archives (then measuring the grass models).
		OpeningArchives,
		// The smooth weight field, collision extraction and the rejection structures of one world.
		Preparing,
		Generating,
		// Every cell is placed; the writer threads are finishing the last files.
		Writing,
		Finished
	};

	// Written by the run, read by anyone (the launcher polls it every frame).
	struct RunProgress
	{
		std::atomic<RunStage> stage{ RunStage::LoadingPlugins };
		// When the current stage began (steady_clock ticks), for the launcher's "time so far".
		std::atomic<std::int64_t> stageBegan{ std::chrono::steady_clock::now().time_since_epoch().count() };
		std::atomic<std::uint32_t> worldIndex{ 0 };
		std::atomic<std::uint32_t> worldCount{ 0 };
		std::atomic<std::uint32_t> worldFormID{ 0 };
		std::atomic<std::uint32_t> cellsDone{ 0 };
		std::atomic<std::uint32_t> cellsTotal{ 0 };
	};

	// The launcher's progress text for a stage. a_seconds is the time in the stage so far; a_world is the
	// "1/3 Tamriel" prefix. Stages that cannot report a fraction end with the seconds; the cell count is
	// its own sign of life.
	[[nodiscard]] std::string StageLabel(RunStage a_stage, std::uint32_t a_seconds, std::string_view a_world, std::uint32_t a_cellsDone, std::uint32_t a_cellsTotal);

	// Whole seconds the run has been in its current stage.
	[[nodiscard]] std::uint32_t SecondsInStage(const RunProgress& a_progress);

	struct RunControl
	{
		// Checked between stages and before each cell; a stopped run ends with cancelled set.
		std::stop_token stop;
		RunProgress* progress{ nullptr };
		// A snapshot already loaded from the same data path and plugins.txt, to skip loading it again.
		const LoadedPlugins* preloaded{ nullptr };
	};

	struct RunResult
	{
		// The process exit code: 0, or 1 when any cell failed or the run was cancelled.
		int exitCode{ 0 };
		bool cancelled{ false };
		std::uint32_t worlds{ 0 };
		std::uint64_t cellsWritten{ 0 };
		std::uint64_t cellsSkipped{ 0 };
		// Cells with no grass, left without a file.
		std::uint64_t cellsEmpty{ 0 };
		std::uint64_t cellsFailed{ 0 };
	};

	struct WorldSummary
	{
		GameData::FormID formID;
		std::string editorID;
		std::size_t cells{ 0 };
		// The worldspace's No Grass flag: the game grows none there, so runs leave it out.
		bool noGrass{ false };
	};

	// Parses every plugin in the load order into the static world snapshot. Throws on a missing
	// plugins.txt or an unreadable plugin.
	[[nodiscard]] LoadedPlugins LoadStaticSnapshot(const GenerateOptions& a_options);

	// The worldspaces with LAND records, in form ID order: what --world all generates.
	[[nodiscard]] std::vector<WorldSummary> ListWorlds(const GameData::StaticWorldSnapshot& a_snapshot);

	// Generates the caches (or runs the diagnostic a_options selects). Throws on invalid input.
	RunResult Run(const GenerateOptions& a_options, const RunControl& a_control = {});
}
