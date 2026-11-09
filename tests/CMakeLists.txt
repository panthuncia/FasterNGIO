find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# One file per module; TestSupport.h has the shared temp-folder and file helpers.
add_executable(FasterNGIOTests
	CellCacheTests.cpp
	CellPipelineTests.cpp
	CollisionTests.cpp
	CommandLineTests.cpp
	FileWriterPoolTests.cpp
	GameDataTests.cpp
	GameIniTests.cpp
	GameInstallTests.cpp
	GeometryTests.cpp
	IniFileTests.cpp
	ModOrganizerTests.cpp
	MpscQueueTests.cpp
	NgioCacheWriterTests.cpp
	NgioConfigTests.cpp
	NgioRulesTests.cpp
	PlacementTests.cpp
	RejectionTests.cpp
	RunStageTests.cpp
	SeasonsTests.cpp
	TestSupport.h
	UserSettingsTests.cpp
)
fasterngio_configure_target(FasterNGIOTests)
# zlib writes the compressed records of the test plugins.
target_link_libraries(FasterNGIOTests PRIVATE FasterNGIOApp ZLIB::ZLIB GTest::gtest_main)
if(FASTERNGIO_ENABLE_GPU AND WIN32)
	fasterngio_enable_agility(FasterNGIOTests)
endif()
gtest_discover_tests(FasterNGIOTests DISCOVERY_MODE PRE_TEST)
