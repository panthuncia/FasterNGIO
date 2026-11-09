#include "Gui/Launcher.h"

#include "Generate.h"
#include "Gui/BackgroundTask.h"
#include "Gui/LauncherSettings.h"
#include "Gui/LogSink.h"
#include "Gui/Window.h"
#include "NgioConfig.h"
#include "SeasonsConfig.h"
#include "Platform/FileSystem.h"
#include "Platform/GameInstall.h"
#include "Platform/ModOrganizer.h"
#include "Platform/Text.h"
#include "Platform/UserSettings.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace FasterNGIO::Gui
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
		constexpr const char* kExampleGameFolder = "e.g. C:\\Games\\Skyrim Special Edition";
		constexpr const char* kDataGrass = "\\Data\\Grass";
#else
		constexpr const char* kExampleGameFolder = "e.g. /home/you/Games/Skyrim Special Edition";
		constexpr const char* kDataGrass = "/Data/Grass";
#endif

		using Platform::IsDirectory;
		using Platform::IsFile;
		using Platform::PathFromUtf8;
		using Platform::Utf8;

		// Enabled plugins are the lines plugins.txt marks with '*'; the base game's masters load
		// whether listed or not.
		[[nodiscard]] std::size_t CountEnabledPlugins(const std::filesystem::path& a_pluginsTxt)
		{
			std::ifstream file(a_pluginsTxt);
			std::size_t count = 0;
			for (std::string line; std::getline(file, line);) {
				count += !line.empty() && line.front() == '*' ? 1 : 0;
			}
			return count;
		}

		[[nodiscard]] std::string FormatSeconds(double a_seconds)
		{
			const auto total = static_cast<long long>(a_seconds + 0.5);
			return total >= 60 ? std::format("{}m {:02}s", total / 60, total % 60) : std::format("{}s", total);
		}

		// The plugins read for the worldspace list, kept for the run when its inputs still match.
		struct Scan
		{
			std::shared_ptr<const App::LoadedPlugins> world;
			std::vector<App::WorldSummary> worlds;
		};

		struct ScanKey
		{
			std::filesystem::path data;
			std::filesystem::path pluginsTxt;
			bool operator==(const ScanKey&) const = default;
		};

		class Launcher
		{
		public:
			Launcher() :
				_sink(std::make_shared<QueueSink>()),
				_settingsPath(Platform::UserSettings::DefaultPath())
			{
				spdlog::default_logger()->sinks().push_back(_sink);
				LoadSettings();
				DetectModOrganizer();
			}

			~Launcher()
			{
				auto& sinks = spdlog::default_logger()->sinks();
				std::erase(sinks, std::static_pointer_cast<spdlog::sinks::sink>(_sink));
			}

			void Setup(float a_dpiScale)
			{
				auto& io = ImGui::GetIO();
				// Nothing written next to wherever the program was started from.
				io.IniFilename = nullptr;
				ImGui::StyleColorsDark();
				auto& style = ImGui::GetStyle();
				style.WindowRounding = 0.0f;
				style.FrameRounding = 4.0f;
				style.GrabRounding = 4.0f;
				style.FramePadding = ImVec2(8.0f, 5.0f);
				style.ItemSpacing = ImVec2(10.0f, 8.0f);
				style.ScaleAllSizes(a_dpiScale);
				style.FontScaleDpi = a_dpiScale;
				if (const auto font = SystemFontPath(); IsFile(font)) {
					io.Fonts->AddFontFromFileTTF(Utf8(font).c_str(), 17.0f);
				} else {
					io.Fonts->AddFontDefault();
				}
			}

			bool Frame()
			{
				DrainLog();
				UpdateInspection();
				UpdateScan();
				UpdateRun();
				Draw();
				return !(_closeAfterCancel && !_run);
			}

			bool CloseRequested()
			{
				SaveSettings();
				if (_run) {
					// Let the run stop at its next cell, then close.
					_run->RequestStop();
					_closeAfterCancel = true;
					return false;
				}
				return true;
			}

			[[nodiscard]] int ExitCode() const { return _lastExitCode; }

		private:
			// --- State derived from the inputs -------------------------------------------------

			[[nodiscard]] std::filesystem::path PluginsTxt() const
			{
				if (!_inputs.pluginsTxt.empty()) {
					return PathFromUtf8(_inputs.pluginsTxt);
				}
				return _install ? _install->pluginsTxt : std::filesystem::path{};
			}

			[[nodiscard]] std::filesystem::path OutputFolder() const
			{
				if (!_inputs.outputFolder.empty()) {
					return PathFromUtf8(_inputs.outputFolder);
				}
				return _install ? _install->data / "Grass" : std::filesystem::path{};
			}

			[[nodiscard]] std::optional<std::filesystem::path> IniFolder() const
			{
				if (!_inputs.gameIniFolder.empty()) {
					return PathFromUtf8(_inputs.gameIniFolder);
				}
				if (_install && IsDirectory(_install->iniDirectory)) {
					return _install->iniDirectory;
				}
				// The usual search (My Games), which finds nothing worse than engine defaults.
				return std::nullopt;
			}

			[[nodiscard]] bool PluginsTxtFound() const { return IsFile(PluginsTxt()); }

			[[nodiscard]] std::optional<ScanKey> CurrentScanKey() const
			{
				if (!_install || !_pluginsFound) {
					return std::nullopt;
				}
				return ScanKey{ _install->data, PluginsTxt() };
			}

			// Re-validates the game folder when an input changed; only filesystem checks.
			void UpdateInspection()
			{
				const auto signature = (_mo2 ? std::string("\x02") : _inputs.gameFolder) + '\n' + _inputs.pluginsTxt;
				if (signature == _inspectedSignature) {
					// plugins.txt appears once the game or a mod manager has run; look again now and then.
					if (_install && !_pluginsFound && Clock::now() - _lastPluginsCheck > std::chrono::seconds(2)) {
						_lastPluginsCheck = Clock::now();
						_pluginsFound = PluginsTxtFound();
						_enabledPlugins = _pluginsFound ? CountEnabledPlugins(PluginsTxt()) : 0;
					}
					return;
				}
				_inspectedSignature = signature;
				_lastEdit = Clock::now();
				_install.reset();
				_installError.clear();
				if (_mo2) {
					_install = _mo2->game;
				} else {
					if (_inputs.gameFolder.empty()) {
						return;
					}
					auto inspected = Platform::InspectGameFolder(PathFromUtf8(_inputs.gameFolder));
					if (!inspected) {
						_installError = inspected.error();
						return;
					}
					_install = std::move(*inspected);
				}
				_pluginsFound = PluginsTxtFound();
				_enabledPlugins = _pluginsFound ? CountEnabledPlugins(PluginsTxt()) : 0;
				DescribeNgioSettings();
				DescribeSeasonsSettings();
			}

			void DescribeSeasonsSettings()
			{
				if (!_install) {
					_seasonsSummary.clear();
					return;
				}
				_seasonsSummary = App::DescribeSeasons(App::ReadSeasonsSettings(_install->data, _inputs.seasons));
			}

			// What NGIO's own settings (GrassControl.ini in the game's Data folder, which Mod Organizer 2
			// shows through its VFS) will change, for the Options section.
			void DescribeNgioSettings()
			{
				const auto settings = App::LoadNgioSettings(App::DefaultNgioConfigPath(_install->data), _install->data);
				_ngioSkipWorlds = settings.skipWorldspaces;
				_ngioOnlyWorlds = settings.onlyWorldspaces;
				if (!settings.Present()) {
					_ngioSummary = "NGIO settings: none found (no SKSE/Plugins/GrassControl.ini in Data), so NGIO's defaults are not applied.";
					return;
				}
				const auto lists = settings.ignoreForms.size() + settings.ignoreGrassForms.size() + settings.textureForms.size();
				_ngioSummary = std::format("NGIO settings from {}: ray cast {}, grass cliffs {}{}{}{}.", Utf8(settings.files.front()), settings.rayCast ? "on" : "off",
					settings.grassCliffs ? "on" : "off", lists ? std::format(", {} ignore/texture form(s)", lists) : std::string{},
					settings.files.size() > 1 ? std::format(", {} object file(s)", settings.files.size() - 1) : std::string{},
					settings.globalGrassScale != 1.0f ? std::format(", global scale {}", settings.globalGrassScale) : std::string{});
			}

			// Reads the plugins in the background for the worldspace list once the inputs have
			// settled; the run reuses the result.
			void UpdateScan()
			{
				std::erase_if(_retiredScans, [](const auto& a_task) { return a_task->Finished(); });
				if (_scan && _scan->Failed() && !_scanErrorLogged) {
					spdlog::error("could not read the plugins: {}", _scan->Error());
					_scanErrorLogged = true;
				}
				const auto key = CurrentScanKey();
				if (_scan && (!key || *key != _scanKey)) {
					_scan->RequestStop();
					_retiredScans.push_back(std::move(_scan));
				}
				if (_scan || !key || _run || Clock::now() - _lastEdit < std::chrono::milliseconds(400)) {
					return;
				}
				_scanKey = *key;
				_scanErrorLogged = false;
				App::GenerateOptions options;
				options.dataPath = key->data;
				options.pluginsTxtPath = key->pluginsTxt;
				_scan = std::make_unique<BackgroundTask<Scan>>([options](std::stop_token) {
					auto world = std::make_shared<App::LoadedPlugins>(App::LoadStaticSnapshot(options));
					auto worlds = App::ListWorlds(world->snapshot);
					return Scan{ std::move(world), std::move(worlds) };
				});
			}

			[[nodiscard]] const Scan* FinishedScan() const
			{
				if (_scan && _scan->Finished() && !_scan->Failed()) {
					return std::addressof(*_scan->Value());
				}
				return nullptr;
			}

			[[nodiscard]] std::string WorldName(std::uint32_t a_formID) const
			{
				if (const auto* scan = FinishedScan()) {
					for (const auto& world : scan->worlds) {
						if (world.formID.value == a_formID) {
							return world.editorID;
						}
					}
				}
				return std::format("{:08X}", a_formID);
			}

			// --- Worldspace selection -----------------------------------------------------------

			// NGIO's Skip-/Only-pregenerate-world-spaces leave this worldspace out of "all", as the run
			// does (App::Run's world selection).
			[[nodiscard]] bool SkippedByNgio(const App::WorldSummary& a_world) const
			{
				const auto listed = [&](const std::vector<std::string>& a_list) {
					return !a_world.editorID.empty() && std::ranges::any_of(a_list, [&](const std::string& a_name) { return Platform::IEquals(a_name, a_world.editorID); });
				};
				return _ngioOnlyWorlds.empty() ? listed(_ngioSkipWorlds) : !listed(_ngioOnlyWorlds);
			}

			// A worldspace flagged No Grass is never generated (App::Run leaves it out).
			[[nodiscard]] bool IsWorldSelected(const App::WorldSummary& a_world) const
			{
				if (a_world.noGrass) {
					return false;
				}
				if (_inputs.allWorlds) {
					return !SkippedByNgio(a_world);
				}
				return std::ranges::find(_inputs.worlds, a_world.formID.value) != _inputs.worlds.end();
			}

			[[nodiscard]] std::size_t NgioSkippedCount(const Scan& a_scan) const
			{
				return static_cast<std::size_t>(std::ranges::count_if(a_scan.worlds, [&](const App::WorldSummary& a_world) { return !a_world.noGrass && SkippedByNgio(a_world); }));
			}

			void SetWorldSelected(const Scan& a_scan, std::uint32_t a_formID, bool a_selected)
			{
				if (_inputs.allWorlds) {
					// Leaving "all": every worldspace it covered, changed by this one.
					_inputs.allWorlds = false;
					_inputs.worlds.clear();
					for (const auto& world : a_scan.worlds) {
						if (!SkippedByNgio(world) && !world.noGrass) {
							_inputs.worlds.push_back(world.formID.value);
						}
					}
				}
				std::erase(_inputs.worlds, a_formID);
				if (a_selected) {
					_inputs.worlds.push_back(a_formID);
				}
			}

			// The selected worldspaces this load order has, in form ID order (all of the saved ones
			// until the plugins are read); empty when all are selected.
			[[nodiscard]] std::vector<GameData::FormID> SelectedWorlds() const
			{
				std::vector<GameData::FormID> worlds;
				if (_inputs.allWorlds) {
					return worlds;
				}
				if (const auto* scan = FinishedScan()) {
					for (const auto& world : scan->worlds) {
						if (IsWorldSelected(world)) {
							worlds.push_back(world.formID);
						}
					}
				} else {
					for (const auto formID : _inputs.worlds) {
						worlds.push_back(GameData::FormID{ formID });
					}
					std::ranges::sort(worlds, {}, &GameData::FormID::value);
				}
				return worlds;
			}

			[[nodiscard]] bool HasWorlds() const { return _inputs.allWorlds || !SelectedWorlds().empty(); }

			[[nodiscard]] std::string WorldsPreview() const
			{
				if (_inputs.allWorlds) {
					if (const auto* scan = FinishedScan()) {
						if (const auto skipped = NgioSkippedCount(*scan); skipped != 0) {
							return std::format("All worldspaces ({} skipped by the NGIO settings)", skipped);
						}
					}
					return "All worldspaces";
				}
				const auto worlds = SelectedWorlds();
				if (worlds.empty()) {
					return "No worldspace selected";
				}
				if (worlds.size() > 3) {
					return std::format("{} worldspaces", worlds.size());
				}
				std::string preview;
				for (const auto world : worlds) {
					preview += (preview.empty() ? "" : ", ") + WorldName(world.value);
				}
				return preview;
			}

			// --- Running ------------------------------------------------------------------------

			[[nodiscard]] bool CanGenerate() const { return !_run && _install && _pluginsFound && !OutputFolder().empty() && HasWorlds(); }

			void StartRun()
			{
				SaveSettings();
				App::GenerateOptions options;
				options.dataPath = _install->data;
				options.pluginsTxtPath = PluginsTxt();
				options.outputDirectory = OutputFolder();
				options.allWorlds = _inputs.allWorlds;
				if (!_inputs.allWorlds) {
					options.worlds = SelectedWorlds();
				}
				options.placement.mode = _inputs.placement;
				options.rejection = _inputs.rejection;
				options.rejectionChosen = true;
				options.overwrite = _inputs.overwrite;
				options.skipEmptyCells = _inputs.skipEmptyCells;
				options.capQuadrantBlades = _inputs.capQuadrantBlades;
				options.renderGeometry = _inputs.renderGeometry;
				options.seasons = _inputs.seasons;
				options.gameIniDirectory = IniFolder();

				std::shared_ptr<const App::LoadedPlugins> preloaded;
				if (const auto* scan = FinishedScan(); scan && CurrentScanKey() == _scanKey) {
					preloaded = scan->world;
				}
				_progress = std::make_unique<App::RunProgress>();
				_runStarted = Clock::now();
				_summary.clear();
				_summaryIsError = false;
				_ranOutput = options.outputDirectory;
				spdlog::info("generating into {}", Platform::Utf8(options.outputDirectory));
				_run = std::make_unique<BackgroundTask<App::RunResult>>(
					[options, preloaded, progress = _progress.get()](std::stop_token a_stop) {
						return App::Run(options, App::RunControl{ .stop = a_stop, .progress = progress, .preloaded = preloaded.get() });
					});
			}

			void UpdateRun()
			{
				if (!_run || !_run->Finished()) {
					return;
				}
				const auto elapsed = FormatSeconds(std::chrono::duration<double>(Clock::now() - _runStarted).count());
				if (_run->Failed()) {
					_summary = "Failed: " + _run->Error();
					_summaryIsError = true;
					_lastExitCode = 1;
					spdlog::error("{}", _run->Error());
				} else {
					const auto& result = *_run->Value();
					_lastExitCode = result.exitCode;
					if (result.cancelled) {
						_summary = std::format("Cancelled after {}: wrote {} cache file(s).", elapsed, result.cellsWritten);
						_summaryIsError = true;
					} else {
						_summary = std::format("Done in {}: wrote {} cache file(s) for {} worldspace(s){}{}{}.", elapsed, result.cellsWritten, result.worlds,
							result.cellsSkipped ? std::format(", kept {} existing", result.cellsSkipped) : std::string{},
							result.cellsEmpty ? std::format(", skipped {} empty", result.cellsEmpty) : std::string{},
							result.cellsFailed ? std::format(", {} failed (see the log)", result.cellsFailed) : std::string{});
						_summaryIsError = result.cellsFailed != 0;
					}
				}
				_run.reset();
			}

			// --- Mod Organizer 2 ----------------------------------------------------------------

			// Started from MO2, the game, load order and INIs come from its instance and profile, as
			// xEdit's do; the remembered game folder is left alone for runs outside MO2.
			void DetectModOrganizer()
			{
				const auto detected = Platform::DetectModOrganizer();
				if (!detected) {
					return;
				}
				if (*detected) {
					_mo2 = **detected;
					spdlog::info("started from Mod Organizer 2: {} ({})", _mo2->Describe(), Utf8(_mo2->game.root));
				} else {
					_mo2Problem = detected->error();
					spdlog::warn("started from Mod Organizer 2, but its instance could not be read: {}", _mo2Problem);
				}
			}

			// --- Settings -----------------------------------------------------------------------

			void LoadSettings()
			{
				_inputs = LauncherSettings::Load(_settingsPath);
				_showAdvanced = !_inputs.pluginsTxt.empty() || !_inputs.gameIniFolder.empty();
			}

			void SaveSettings() const { _inputs.Save(_settingsPath); }

			// --- Log ----------------------------------------------------------------------------

			void DrainLog()
			{
				constexpr std::size_t kMaxLines = 4000;
				while (auto line = _sink->TryPop()) {
					_log.push_back(std::move(*line));
					_scrollToBottom = true;
				}
				while (_log.size() > kMaxLines) {
					_log.pop_front();
				}
			}

			// --- Drawing ------------------------------------------------------------------------

			// A text field with a Browse button that opens the folder chooser.
			bool FolderField(const char* a_id, std::string& a_text, const char* a_hint, const char* a_pickerTitle)
			{
				const auto& style = ImGui::GetStyle();
				const float buttonWidth = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - style.ItemSpacing.x);
				bool changed = ImGui::InputTextWithHint(a_id, a_hint, &a_text);
				ImGui::SameLine();
				ImGui::PushID(a_id);
				ImGui::BeginDisabled(!CanPickFolder());
				if (ImGui::Button("Browse...")) {
					const auto start = a_text.empty() ? std::filesystem::path{} : PathFromUtf8(a_text);
					if (const auto picked = PickFolder(a_pickerTitle, start)) {
						a_text = Utf8(*picked);
						changed = true;
					}
				}
				ImGui::EndDisabled();
				ImGui::PopID();
				return changed;
			}

			static void StatusLine(bool a_ok, const std::string& a_text)
			{
				const ImVec4 color = a_ok ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.55f, 0.4f, 1.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextWrapped("%s %s", a_ok ? "OK:" : "!", a_text.c_str());
				ImGui::PopStyleColor();
			}

			void DrawGame()
			{
				ImGui::SeparatorText("Skyrim Special Edition");
				if (_mo2) {
					StatusLine(true, std::format("Mod Organizer 2: {}. {} version in {}. Load order: {} enabled plugin(s).", _mo2->Describe(),
										 Platform::GameStoreName(_mo2->game.store), Utf8(_mo2->game.root), _enabledPlugins));
					if (ImGui::SmallButton("Choose a game folder instead")) {
						_mo2.reset();
					}
					return;
				}
				if (!_mo2Problem.empty()) {
					StatusLine(false, std::format("Started from Mod Organizer 2, but its instance could not be read: {}", _mo2Problem));
				}
				ImGui::TextUnformatted("Game folder (the one with SkyrimSE.exe):");
				FolderField("##game", _inputs.gameFolder, kExampleGameFolder, "Choose the Skyrim Special Edition folder");
				if (_inputs.gameFolder.empty()) {
					ImGui::TextDisabled("Choose the folder Skyrim Special Edition is installed in.");
				} else if (!_install) {
					StatusLine(false, _installError);
				} else if (!_pluginsFound) {
					StatusLine(false, std::format("{} ({}), but its load order was not found at {}. Start the game (or your mod manager) once, "
												  "or choose plugins.txt under Advanced.",
						Utf8(_install->root), Platform::GameStoreName(_install->store), Utf8(PluginsTxt())));
				} else {
					StatusLine(true, std::format("{} version. Load order: {} enabled plugin(s) from {}", Platform::GameStoreName(_install->store), _enabledPlugins,
										 Utf8(PluginsTxt())));
				}
			}

			void DrawOutput()
			{
				ImGui::SeparatorText("Output");
				const auto defaultOutput = _install ? Utf8(_install->data / "Grass") : std::string("<game folder>") + kDataGrass;
				FolderField("##output", _inputs.outputFolder, defaultOutput.c_str(), "Choose where to write the grass cache");
				ImGui::TextDisabled("Leave empty for %s, where NGIO's own pregeneration writes and where NGIO and DynDOLOD read the cache.", kDataGrass + 1);
				if (_mo2) {
					ImGui::TextDisabled("Under Mod Organizer 2, new files go to its Overwrite folder (or the mod chosen in this executable's settings).");
				}
			}

			void DrawOptions()
			{
				ImGui::SeparatorText("Options");
				const float labelWidth = ImGui::CalcTextSize("Grass in objects").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
				const float fieldWidth = (std::min)(ImGui::GetContentRegionAvail().x - labelWidth, 480.0f * ImGui::GetStyle().FontScaleDpi);

				// Worldspaces.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Worldspaces");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				const auto* scan = FinishedScan();
				if (ImGui::BeginCombo("##world", WorldsPreview().c_str(), ImGuiComboFlags_HeightLarge)) {
					// Checkboxes, which leave the list open for the next one. "All" checks every box;
					// unchecking one of them then leaves the rest.
					bool all = _inputs.allWorlds;
					if (ImGui::Checkbox(scan && NgioSkippedCount(*scan) != 0 ? "All worldspaces, except those GrassControl.ini skips" : "All worldspaces", &all)) {
						_inputs.allWorlds = all;
						_inputs.worlds.clear();
					}
					if (scan) {
						ImGui::Separator();
						for (const auto& world : scan->worlds) {
							const auto label = std::format("{}  ({} cells){}##{:08X}", world.editorID, world.cells,
								world.noGrass ? ", flagged No Grass: the game grows none"
								: SkippedByNgio(world) ? ", skipped by GrassControl.ini unless ticked" : "", world.formID.value);
							bool selected = IsWorldSelected(world);
							ImGui::BeginDisabled(world.noGrass);
							if (ImGui::Checkbox(label.c_str(), &selected)) {
								SetWorldSelected(*scan, world.formID.value, selected);
							}
							ImGui::EndDisabled();
						}
					} else if (_scan && _scan->Failed()) {
						ImGui::TextDisabled("Could not read the plugins: %s", _scan->Error().c_str());
					} else if (_scan) {
						ImGui::TextDisabled("Reading the plugins...");
					} else {
						ImGui::TextDisabled("Choose the game folder to list its worldspaces.");
					}
					ImGui::EndCombo();
				}

				// Placement.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Placement");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				constexpr const char* kPlacements[] = { "Smooth (follows the painted terrain)", "Vanilla (identical to the game's)" };
				int placement = _inputs.placement == Grass::PlacementMode::Vanilla ? 1 : 0;
				if (ImGui::Combo("##placement", &placement, kPlacements, IM_ARRAYSIZE(kPlacements))) {
					_inputs.placement = placement == 1 ? Grass::PlacementMode::Vanilla : Grass::PlacementMode::Smooth;
				}

				// Rejection.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				struct Choice
				{
					App::RejectChoice value;
					const char* label;
				};
				ImGui::Checkbox("Use model geometry instead of collision (experimental)", &_inputs.renderGeometry);
				_inputs.rejection = &_inputs.renderGeometry ? FasterNGIO::App::RejectChoice::Gpu : FasterNGIO::App::RejectChoice::Cpu;
				
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip(
						"Objects that have collision remove grass where their visible mesh is, instead of where\n"
						"their (usually coarser) collision is. Objects without collision still keep grass.");
				}

				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted("Seasons of Skyrim");
				ImGui::SameLine(labelWidth);
				ImGui::SetNextItemWidth(fieldWidth);
				{
					static constexpr std::pair<App::SeasonsChoice, const char*> kSeasonChoices[] = {
						{ App::SeasonsChoice::Auto, "Seasonal caches when Seasons of Skyrim is installed" },
						{ App::SeasonsChoice::On, "Always write seasonal caches" },
						{ App::SeasonsChoice::Off, "Plain caches only" },
					};
					const char* currentSeasons = kSeasonChoices[0].second;
					for (const auto& [value, label] : kSeasonChoices) {
						if (value == _inputs.seasons) {
							currentSeasons = label;
						}
					}
					if (ImGui::BeginCombo("##seasons", currentSeasons)) {
						for (const auto& [value, label] : kSeasonChoices) {
							if (ImGui::Selectable(label, value == _inputs.seasons) && value != _inputs.seasons) {
								_inputs.seasons = value;
								DescribeSeasonsSettings();
							}
						}
						ImGui::EndCombo();
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip(
							"Grass Cache Helper NG loads <cell>.WIN.cgid etc. for the current season. Each season's grass\n"
							"and object swaps are applied as Seasons of Skyrim applies them in-game.");
					}
				}

				ImGui::Checkbox("Rebuild cache files that already exist", &_inputs.overwrite);
				if (_install && !_ngioSummary.empty()) {
					ImGui::TextDisabled("%s", _ngioSummary.c_str());
				}
				if (_install && !_seasonsSummary.empty()) {
					ImGui::TextDisabled("%s", _seasonsSummary.c_str());
				}

				ImGui::SetNextItemOpen(_showAdvanced, ImGuiCond_Once);
				if (ImGui::TreeNode("Advanced")) {
					ImGui::TextUnformatted("Load order (plugins.txt):");
					const auto pluginsHint = _install ? Utf8(_install->pluginsTxt) : std::string("derived from the game folder");
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputTextWithHint("##plugins", pluginsHint.c_str(), &_inputs.pluginsTxt);
					ImGui::TextUnformatted("Game INI folder (Skyrim.ini's [Grass] settings):");
					const auto iniHint = _install ? Utf8(_install->iniDirectory) : std::string("derived from the game folder");
					FolderField("##ini", _inputs.gameIniFolder, iniHint.c_str(), "Choose the folder with Skyrim.ini");
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted("Cells with no grass:");
					ImGui::SameLine();
					constexpr const char* kEmptyCells[] = { "Automatic (from NGIO's settings)", "Write an empty cache file", "Write no file" };
					int emptyCells = !_inputs.skipEmptyCells ? 0 : *_inputs.skipEmptyCells ? 2 : 1;
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::Combo("##emptycells", &emptyCells, kEmptyCells, IM_ARRAYSIZE(kEmptyCells))) {
						_inputs.skipEmptyCells = emptyCells == 0 ? std::nullopt : std::optional<bool>(emptyCells == 2);
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip(
							"Without a cache file the game generates a cell's grass itself, every time it loads, unless NGIO\n"
							"only loads from the cache (Use-grass-cache and Only-load-from-cache). Automatic writes NGIO's\n"
							"4-byte empty file unless it does.");
					}
					ImGui::Checkbox("Limit grass to the game's 8191 blades per type and cell quadrant", &_inputs.capQuadrantBlades);
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip(
							"The game keeps at most 8191 blades of one grass type in a quarter of a cell when it generates\n"
							"grass. With this on, denser grass is thinned evenly to that limit; off, every blade is kept.");
					}
					ImGui::TreePop();
				}
			}

			void DrawRun()
			{
				ImGui::Spacing();
				const auto buttonSize = ImVec2(160.0f * ImGui::GetStyle().FontScaleDpi, 0.0f);
				if (_run) {
					ImGui::BeginDisabled(_run && _closeAfterCancel);
					if (ImGui::Button(_closeAfterCancel ? "Cancelling..." : "Cancel", buttonSize)) {
						_run->RequestStop();
					}
					ImGui::EndDisabled();
				} else {
					ImGui::BeginDisabled(!CanGenerate());
					if (ImGui::Button("Generate", buttonSize)) {
						StartRun();
					}
					ImGui::EndDisabled();
				}
				// Beside the button: the progress, the last run's outcome, or why it is disabled.
				if ((_run && _progress) || !_summary.empty() || !CanGenerate()) {
					ImGui::SameLine();
				}

				if (_run && _progress) {
					const auto stage = _progress->stage.load(std::memory_order_relaxed);
					const auto worldCount = _progress->worldCount.load(std::memory_order_relaxed);
					const auto worldIndex = _progress->worldIndex.load(std::memory_order_relaxed);
					const auto done = _progress->cellsDone.load(std::memory_order_relaxed);
					const auto total = _progress->cellsTotal.load(std::memory_order_relaxed);
					float fraction = 0.0f;
					const auto label = App::StageLabel(stage, App::SecondsInStage(*_progress), std::format("{}/{} {}", worldIndex + 1, worldCount, WorldName(_progress->worldFormID.load())), done, total);
					if (worldCount != 0) {
						const float worldFraction = total != 0 && stage >= App::RunStage::Generating ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
						fraction = (static_cast<float>(worldIndex) + worldFraction) / static_cast<float>(worldCount);
					}
					ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), label.c_str());
				} else if (!_summary.empty()) {
					ImGui::AlignTextToFramePadding();
					StatusLine(!_summaryIsError, _summary);
				} else if (!CanGenerate()) {
					ImGui::AlignTextToFramePadding();
					const char* reason = "Choose a valid game folder to start.";
					if (_install && !_pluginsFound) {
						reason = "The load order (plugins.txt) is needed to start.";
					} else if (_install && !HasWorlds()) {
						reason = "Choose at least one worldspace to start.";
					}
					ImGui::TextDisabled("%s", reason);
				}
				if (!_run && !_summary.empty() && IsDirectory(_ranOutput)) {
					if (ImGui::Button("Open output folder", buttonSize)) {
						OpenFolder(_ranOutput);
					}
				}
			}

			void DrawLog()
			{
				ImGui::SeparatorText("Log");
				if (ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
					const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(_log.size()));
					while (clipper.Step()) {
						for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
							const auto& line = _log[static_cast<std::size_t>(i)];
							const bool colored = line.level >= spdlog::level::warn;
							if (colored) {
								ImGui::PushStyleColor(ImGuiCol_Text, line.level >= spdlog::level::err ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
							}
							ImGui::TextUnformatted(line.text.c_str(), line.text.c_str() + line.text.size());
							if (colored) {
								ImGui::PopStyleColor();
							}
						}
					}
					if (_scrollToBottom && atBottom) {
						ImGui::SetScrollHereY(1.0f);
					}
					_scrollToBottom = false;
				}
				ImGui::EndChild();
			}

			void Draw()
			{
				const auto* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
				ImGui::Begin("FasterNGIO", nullptr, flags);
				ImGui::TextDisabled("Generates the grass cache No Grass In Objects and DynDOLOD use, without starting the game.");
				ImGui::BeginDisabled(_run != nullptr);
				DrawGame();
				DrawOutput();
				DrawOptions();
				ImGui::EndDisabled();
				DrawRun();
				DrawLog();
				ImGui::End();
			}

			std::shared_ptr<QueueSink> _sink;
			std::filesystem::path _settingsPath;

			LauncherSettings _inputs;
			// Set when started from Mod Organizer 2 (until the user chooses a folder instead); the
			// game folder field and its saved value are then not used.
			std::optional<Platform::Mo2Instance> _mo2;
			std::string _mo2Problem;
			bool _showAdvanced{ false };

			// Derived from the inputs.
			std::string _inspectedSignature{ "\x01" };
			Clock::time_point _lastEdit{};
			std::optional<Platform::GameInstall> _install;
			std::string _installError;
			bool _pluginsFound{ false };
			Clock::time_point _lastPluginsCheck{};
			std::size_t _enabledPlugins{ 0 };
			std::string _ngioSummary;
			// GrassControl.ini's Skip-/Only-pregenerate-world-spaces.
			std::vector<std::string> _ngioSkipWorlds;
			std::vector<std::string> _ngioOnlyWorlds;
			std::string _seasonsSummary;

			std::unique_ptr<BackgroundTask<Scan>> _scan;
			ScanKey _scanKey;
			bool _scanErrorLogged{ false };
			std::vector<std::unique_ptr<BackgroundTask<Scan>>> _retiredScans;

			std::unique_ptr<BackgroundTask<App::RunResult>> _run;
			std::unique_ptr<App::RunProgress> _progress;
			Clock::time_point _runStarted{};
			std::filesystem::path _ranOutput;
			std::string _summary;
			bool _summaryIsError{ false };
			bool _closeAfterCancel{ false };
			int _lastExitCode{ 0 };

			std::deque<LogLine> _log;
			bool _scrollToBottom{ false };
		};
	}

	int RunLauncher()
	{
		try {
			Launcher launcher;
			WindowCallbacks callbacks;
			callbacks.setup = [&](float a_dpiScale) { launcher.Setup(a_dpiScale); };
			callbacks.frame = [&] { return launcher.Frame(); };
			callbacks.closeRequested = [&] { return launcher.CloseRequested(); };
			RunWindow("FasterNGIO", 960, 820, callbacks);
			return launcher.ExitCode();
		} catch (const std::exception& e) {
			spdlog::error("{}", e.what());
			spdlog::info("run FasterNGIO --help for the command-line options");
			return 1;
		}
	}
}
