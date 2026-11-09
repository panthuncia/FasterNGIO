#include "Generate.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace
{
	using namespace FasterNGIO::App;

	std::string Label(RunStage a_stage, std::uint32_t a_seconds = 7)
	{
		return StageLabel(a_stage, a_seconds, "2/3 Tamriel", 12, 40);
	}
}

// The order Run moves through: the launcher compares stages (Generating and later show a cell count),
// and a run without Seasons of Skyrim simply skips WorkingOutSeasons.
TEST(RunStage, RunsInTheOrderTheyHappen)
{
	EXPECT_LT(RunStage::LoadingPlugins, RunStage::WorkingOutSeasons);
	EXPECT_LT(RunStage::WorkingOutSeasons, RunStage::OpeningArchives);
	EXPECT_LT(RunStage::OpeningArchives, RunStage::Preparing);
	EXPECT_LT(RunStage::Preparing, RunStage::Generating);
	EXPECT_LT(RunStage::Generating, RunStage::Writing);
	EXPECT_LT(RunStage::Writing, RunStage::Finished);
}

TEST(RunStage, EveryStageWithoutACellCountEndsWithTheSecondsSoFar)
{
	EXPECT_EQ(Label(RunStage::LoadingPlugins), "Reading plugins... 7s");
	EXPECT_EQ(Label(RunStage::WorkingOutSeasons, 23), "Working out the Seasons of Skyrim winter swaps... 23s");
	EXPECT_EQ(Label(RunStage::OpeningArchives, 0), "Opening the game's archives... 0s");
	EXPECT_EQ(Label(RunStage::Preparing), "2/3 Tamriel: preparing collision... 7s");
	EXPECT_EQ(Label(RunStage::Writing), "Writing the last files... 7s");
}

TEST(RunStage, GeneratingShowsTheCellCountAndNoSeconds)
{
	EXPECT_EQ(Label(RunStage::Generating), "2/3 Tamriel: 12/40 cells");
	EXPECT_EQ(Label(RunStage::Finished), "2/3 Tamriel: 12/40 cells");
}

TEST(RunStage, SecondsInStageCountsFromWhenTheStageBegan)
{
	RunProgress progress;
	const auto now = std::chrono::steady_clock::now();
	progress.stageBegan.store((now - std::chrono::seconds(5)).time_since_epoch().count());
	EXPECT_GE(SecondsInStage(progress), 5u);
	EXPECT_LE(SecondsInStage(progress), 6u);
	progress.stageBegan.store((now + std::chrono::seconds(60)).time_since_epoch().count());
	EXPECT_EQ(SecondsInStage(progress), 0u);
}
