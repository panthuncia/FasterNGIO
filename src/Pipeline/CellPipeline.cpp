#include "Pipeline/CellPipeline.h"

#include "Concurrency/AtomicWait.h"
#include "Grass/CellCache.h"
#include "Grass/LandTexture.h"
#include "Pipeline/FileWriterPool.h"
#include "Pipeline/SuspensionWaiters.h"
#include "Pipeline/TbbGraphScheduler.h"
#include "Platform/ModOrganizer.h"
#include "Platform/Text.h"
#include "Platform/WholeFile.h"
#include "Rejection/CpuBvh.h"
#include "Rejection/CpuReference.h"
#if FASTERNGIO_HAS_GPU
#include "Gpu/GpuRejector.h"
#endif

#include <ORGModuleServices/Async/StateGraph.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace FasterNGIO::Pipeline
{
	namespace
	{
		using Collision::Float3;
		using Graph = org::async::AsyncStateGraph;
		using Key = Graph::ArtifactKey;
		using BuildContext = Graph::ArtifactBuildContext;
		using BuildResult = Graph::ArtifactBuildResult;

		constexpr std::uint16_t kCellTrace = 0;
		constexpr std::uint16_t kCellOutput = 1;
		// Between the two when the GPU traces a world with NGIO roles: reads the volume pass and posts
		// the cliff pass for the blades that need it.
		constexpr std::uint16_t kCellCliffs = 2;

		struct CellInput
		{
			std::uint32_t cell{ 0 };
		};

		// Everything one cell carries from placement to output. Owned by the CellTrace payload;
		// CellOutput empties it once the file is written so finished cells hold no memory.
		struct CellWork
		{
			bool skip{ false };
			bool cancelled{ false };
			bool holdsCapacity{ false };
			std::string error;
			Grass::CellCandidates candidates;
			std::vector<Rejection::QueryShape> shapes;
			std::vector<std::uint32_t> rejected;
			// With NGIO's cliffs or ignored shapes in the world: what each blade's volume touched, the
			// blades that need the cliff rays, and those rays (one per candidate).
			std::vector<Rejection::VolumeHits> volumeHits;
			std::vector<std::uint32_t> cliffCandidates;
			std::vector<Rejection::CliffRays> cliffRays;
#if FASTERNGIO_HAS_GPU
			std::shared_ptr<Gpu::TraceJob> job;
			std::shared_ptr<Gpu::TraceJob> cliffJob;
#endif
		};

		struct CellTraceArtifact
		{
			std::shared_ptr<CellWork> work;
		};

		struct CellDone
		{
		};

		// Cells whose candidates may wait for the GPU at once. Producers past the limit suspend in the
		// graph until a cell is written; nothing blocks.
		constexpr std::uint32_t kMaxCellsAwaitingGpu = 4096;

		// Admission for cells waiting on the GPU. TryAcquire never blocks; a producer that misses
		// registers a waiter and suspends in the graph, and Release wakes exactly one live waiter.
		class CapacityBroker
		{
		public:
			CapacityBroker(std::uint32_t a_capacity, std::function<void(std::uint64_t)> a_notify) :
				_available(a_capacity), _notify(std::move(a_notify)) {}

			[[nodiscard]] bool TryAcquire() noexcept
			{
				auto available = _available.load(std::memory_order_acquire);
				while (available > 0) {
					if (_available.compare_exchange_weak(available, available - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
						return true;
					}
				}
				return false;
			}

			// Returns 0 when capacity was obtained after all, else the identity to suspend on.
			[[nodiscard]] std::uint64_t AcquireOrWait()
			{
				if (TryAcquire()) {
					return 0;
				}
				return _waiters.Wait(_notify, [this] { return TryAcquire(); });
			}

			void Release()
			{
				_available.fetch_add(1, std::memory_order_acq_rel);
				_waiters.WakeOne();
			}

		private:
			std::atomic<std::int64_t> _available;
			SuspensionWaiters::Notify _notify;
			SuspensionWaiters _waiters;
		};

		[[nodiscard]] std::uint64_t CountBits(const std::vector<std::uint32_t>& a_bits)
		{
			std::uint64_t count = 0;
			for (const auto word : a_bits) {
				count += static_cast<std::uint64_t>(std::popcount(word));
			}
			return count;
		}

		class CellPipelineRun
		{
		public:
			explicit CellPipelineRun(const CellPipelineDesc& a_desc) :
				_desc(a_desc),
				_graph(MakeTbbGraphScheduler(), "FasterNGIO", MakeHooks()),
				_capacity(kMaxCellsAwaitingGpu, _graph.MakeSuspensionNotifier()),
				_writeNotify(_graph.MakeSuspensionNotifier()),
				_usesRoles(a_desc.backend != RejectionBackend::None && a_desc.features && a_desc.world && Rejection::UsesRoles(*a_desc.world))
			{
				_graph.RegisterTypedProducer<CellInput, CellTraceArtifact>(kCellTrace, 0, 0, "FasterNGIO::CellTrace",
					[this](const BuildContext& a_context, std::shared_ptr<const CellInput> a_input) { return BuildTrace(a_context, *a_input); });
				_graph.RegisterTypedProducer<CellInput, CellDone>(kCellOutput, 0, 0, "FasterNGIO::CellOutput",
					[this](const BuildContext& a_context, std::shared_ptr<const CellInput> a_input) { return BuildOutput(a_context, *a_input); });
#if FASTERNGIO_HAS_GPU
				_graph.RegisterTypedProducer<CellInput, CellTraceArtifact>(kCellCliffs, 0, 0, "FasterNGIO::CellCliffs",
					[this](const BuildContext& a_context, std::shared_ptr<const CellInput> a_input) { return BuildCliffs(a_context, *a_input); });
#endif
			}

			~CellPipelineRun() { _graph.Shutdown(); }

			CellPipelineStats Run()
			{
				const auto cellCount = static_cast<std::uint32_t>(_desc.lands.size());
				// Exact (non-coalescible) lock-free posts: each returns its version handle immediately,
				// so the output can name its trace before the graph has applied either request.
				const bool cliffStage = _usesRoles && _desc.backend == RejectionBackend::Gpu;
				std::vector<Graph::ArtifactRequestResult> requests;
				requests.reserve(static_cast<std::size_t>(cellCount) * (cliffStage ? 3 : 2));
				for (std::uint32_t cell = 0; cell < cellCount; ++cell) {
					auto trace = _graph.PostRequest(Intent(kCellTrace, cell, {}), false);
					if (!trace) {
						throw std::runtime_error("state graph refused a cell trace request");
					}
					auto previous = trace.version;
					requests.push_back(std::move(trace));
					if (cliffStage) {
						auto cliffs = _graph.PostRequest(Intent(kCellCliffs, cell, { org::async::Exact(previous, org::async::ArtifactReadiness::GpuReady) }), false);
						if (!cliffs) {
							throw std::runtime_error("state graph refused a cell cliff request");
						}
						previous = cliffs.version;
						requests.push_back(std::move(cliffs));
					}
					auto output = _graph.PostRequest(Intent(kCellOutput, cell, { org::async::Exact(previous, org::async::ArtifactReadiness::GpuReady) }), false);
					if (!output) {
						throw std::runtime_error("state graph refused a cell output request");
					}
					requests.push_back(std::move(output));
				}

				// Sleep (futex) until every CellOutput has run; producers count themselves done.
				Concurrency::WaitUntil(_finished, [cellCount](auto a_finished) { return a_finished >= cellCount; });

				CellPipelineStats stats;
				stats.cellsWritten = _written.load();
				stats.cellsSkipped = _skipped.load();
				stats.cellsEmpty = _empty.load();
				stats.cellsFailed = _failed.load();
				stats.cellsCancelled = _cancelled.load();
				stats.blades = _blades.load();
				stats.bladesRejected = _rejected.load();
				stats.validationMismatches = _mismatches.load();
				stats.bladesMoved = _moved.load();
				stats.bladesCapped = _capped.load();
				return stats;
			}

		private:
			[[nodiscard]] static org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> MakeHooks()
			{
				org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
				// A cell's trace and output are lifecycle steps of one exact request each.
				hooks.artifactPolicies[kCellTrace].allowCoalescing = false;
				hooks.artifactPolicies[kCellOutput].allowCoalescing = false;
				hooks.artifactPolicies[kCellCliffs].allowCoalescing = false;
				return hooks;
			}

			[[nodiscard]] static Graph::ArtifactIntent Intent(std::uint16_t a_kind, std::uint32_t a_cell, std::vector<Graph::ArtifactRequirement> a_requirements)
			{
				Graph::ArtifactIntent intent;
				intent.key = Key{ a_kind, a_cell, 0 };
				intent.desiredRevision = 1;
				intent.requirements = std::move(a_requirements);
				intent.input = org::async::ArtifactPayload::Make<CellInput>(std::make_shared<const CellInput>(CellInput{ a_cell }));
				intent.requestFingerprint = (static_cast<std::uint64_t>(a_kind) << 32) | (static_cast<std::uint64_t>(a_cell) + 1);
				return intent;
			}

			[[nodiscard]] std::filesystem::path CellPath(std::uint32_t a_cell, std::size_t a_name = 0) const
			{
				const auto& land = *_desc.lands[a_cell];
				return _desc.outputDirectory / Grass::MakeNgioCacheFileName(_desc.worldEditorID, *land.cellX, *land.cellY, _desc.fileSuffixes[a_name]);
			}

			[[nodiscard]] bool AllFilesExist(std::uint32_t a_cell) const
			{
				for (std::size_t name = 0; name < _desc.fileSuffixes.size(); ++name) {
					if (!Grass::ExistingNgioCacheLooksValid(CellPath(a_cell, name))) {
						return false;
					}
				}
				return true;
			}

			[[nodiscard]] static BuildResult Ready(std::shared_ptr<CellWork> a_work, std::shared_ptr<const org::async::GpuSubmissionSet> a_gpu = {})
			{
				return BuildResult::Ready(
					org::async::ArtifactPayload::Make<CellTraceArtifact>(std::make_shared<const CellTraceArtifact>(CellTraceArtifact{ std::move(a_work) })),
					std::move(a_gpu));
			}

			// Placement, then rejection: inline on the CPU, or a trace job posted to the render
			// thread whose completion the graph tracks as this artifact's GPU submission.
			BuildResult BuildTrace(const BuildContext&, const CellInput& a_input)
			{
				auto work = std::make_shared<CellWork>();
				try {
					if (_desc.stop.stop_requested()) {
						work->cancelled = true;
						return Ready(std::move(work));
					}
					if (!_desc.overwrite && AllFilesExist(a_input.cell)) {
						work->skip = true;
						return Ready(std::move(work));
					}
					// Before anything is held: a cell is not placed while its file would only queue behind a
					// full backlog.
					if (_desc.writer) {
						if (const auto identity = _desc.writer->AdmitOrWait(_writeNotify); identity != 0) {
							return BuildResult::Suspend(Graph::ArtifactSuspension::Capacity(identity, "the cache writers are behind"));
						}
					}
					if (_desc.backend == RejectionBackend::Gpu) {
						if (const auto identity = _capacity.AcquireOrWait(); identity != 0) {
							return BuildResult::Suspend(Graph::ArtifactSuspension::Capacity(identity, "cells awaiting the GPU are at their limit"));
						}
						work->holdsCapacity = true;
					}
					work->candidates = Grass::GenerateCellCandidates(*_desc.snapshot, *_desc.lands[a_input.cell], _desc.placement);
					work->shapes.reserve(work->candidates.groups.size());
					for (const auto& group : work->candidates.groups) {
						work->shapes.push_back(_desc.shapesByGrass->at(group.grass->formID));
					}
					if (_desc.backend == RejectionBackend::Cpu && _usesRoles) {
						TraceRolesOnCpu(*work);
					} else if (_desc.backend == RejectionBackend::Cpu) {
						work->rejected = _desc.cpuBvh->RejectCell(work->candidates, work->shapes);
						if (_desc.validateCpu) {
							Validate(*work);
						}
					}
#if FASTERNGIO_HAS_GPU
					if (_desc.backend == RejectionBackend::Gpu) {
						return PostToGpu(std::move(work));
					}
#endif
				} catch (const std::exception& e) {
					work->error = e.what();
				}
				return Ready(std::move(work));
			}

#if FASTERNGIO_HAS_GPU
			BuildResult PostToGpu(std::shared_ptr<CellWork> a_work)
			{
				auto queries = Gpu::MakeQueries(a_work->candidates.blades, a_work->shapes);
				if (queries.empty()) {
					return Ready(std::move(a_work));
				}
				auto job = std::make_shared<Gpu::TraceJob>(std::move(queries));
				auto token = std::make_shared<org::async::GpuSubmissionSet>();
				token->isSubmitted = [] { return true; };
				token->isComplete = [job] { return job->Complete(); };
				token->subscribe = [job](std::function<void()> a_callback) { job->Subscribe(std::move(a_callback)); };
				token->completionNotificationsAreAuthoritative = true;
				a_work->job = job;
				_desc.gpu->Post(std::move(job));
				return Ready(std::move(a_work), std::move(token));
			}

			[[nodiscard]] static std::shared_ptr<org::async::GpuSubmissionSet> CompletionToken(const std::shared_ptr<Gpu::TraceJob>& a_job)
			{
				auto token = std::make_shared<org::async::GpuSubmissionSet>();
				token->isSubmitted = [] { return true; };
				token->isComplete = [a_job] { return a_job->Complete(); };
				token->subscribe = [a_job](std::function<void()> a_callback) { a_job->Subscribe(std::move(a_callback)); };
				token->completionNotificationsAreAuthoritative = true;
				return token;
			}

			// The volume pass's role hits, then the cliff pass for the blades that touched a cliff.
			BuildResult BuildCliffs(const BuildContext& a_context, const CellInput&)
			{
				std::shared_ptr<CellWork> work;
				for (const auto& dependency : a_context.dependencies) {
					if (dependency.key.kind == kCellTrace) {
						if (const auto trace = dependency.payload.Get<CellTraceArtifact>()) {
							work = trace->work;
						}
					}
				}
				if (!work || !work->job || !work->error.empty()) {
					return Ready(std::move(work));
				}
				try {
					if (work->job->Failed()) {
						throw std::runtime_error("GPU trace failed");
					}
					const auto& candidates = work->candidates;
					const auto words = work->job->Hits();
					const auto stride = work->job->ResultWords();
					work->volumeHits.resize(candidates.blades.size());
					std::vector<Gpu::Query> queries;
					for (std::size_t b = 0; b < candidates.blades.size(); ++b) {
						work->volumeHits[b] = Gpu::ReadVolumeHits(words.subspan(b * stride, stride));
						if (Rejection::NeedsCliffRays(work->volumeHits[b], *_desc.features)) {
							const auto& blade = candidates.blades[b];
							work->cliffCandidates.push_back(static_cast<std::uint32_t>(b));
							queries.push_back(Gpu::MakeCliffQuery(blade.position, Rejection::CliffNeighbourOffset(*candidates.groups[blade.groupIndex].grass)));
						}
					}
					if (queries.empty()) {
						return Ready(std::move(work));
					}
					auto job = std::make_shared<Gpu::TraceJob>(std::move(queries), Gpu::TraceKind::Cliff);
					auto token = CompletionToken(job);
					work->cliffJob = job;
					_desc.gpu->Post(std::move(job));
					return Ready(std::move(work), std::move(token));
				} catch (const std::exception& e) {
					work->error = e.what();
				}
				return Ready(std::move(work));
			}
#endif

			void CountFinished()
			{
				if (_desc.progress) {
					_desc.progress->fetch_add(1, std::memory_order_relaxed);
				}
				_finished.fetch_add(1, std::memory_order_acq_rel);
				_finished.notify_all();
			}

			void Finish(CellWork& a_work)
			{
				if (a_work.holdsCapacity) {
					a_work.holdsCapacity = false;
					_capacity.Release();
				}
				a_work = CellWork{};
				CountFinished();
			}

			BuildResult BuildOutput(const BuildContext& a_context, const CellInput& a_input)
			{
				std::shared_ptr<const CellTraceArtifact> trace;
				for (const auto& dependency : a_context.dependencies) {
					if (dependency.key.kind == kCellTrace || dependency.key.kind == kCellCliffs) {
						trace = dependency.payload.Get<CellTraceArtifact>();
					}
				}
				const auto done = BuildResult::Ready(org::async::ArtifactPayload::Make<CellDone>(std::make_shared<const CellDone>()));
				if (!trace || !trace->work) {
					_failed.fetch_add(1);
					CountFinished();
					return done;
				}
				auto& work = *trace->work;
				try {
					if (work.skip) {
						_skipped.fetch_add(1);
						Finish(work);
						return done;
					}
					if (work.cancelled) {
						_cancelled.fetch_add(1);
						Finish(work);
						return done;
					}
#if FASTERNGIO_HAS_GPU
					if (work.cliffJob) {
						if (work.cliffJob->Failed()) {
							throw std::runtime_error("GPU cliff trace failed");
						}
						const auto words = work.cliffJob->Hits();
						work.cliffRays.reserve(work.cliffCandidates.size());
						for (std::size_t i = 0; i < work.cliffCandidates.size(); ++i) {
							work.cliffRays.push_back(Gpu::ReadCliffRays(words.subspan(i * Gpu::kCliffResultWords, Gpu::kCliffResultWords)));
						}
					}
					if (work.job && !_usesRoles) {
						if (work.job->Failed()) {
							throw std::runtime_error("GPU trace failed");
						}
						const auto hits = work.job->Hits();
						work.rejected.assign((work.candidates.blades.size() + 31) / 32, 0u);
						for (std::size_t b = 0; b < hits.size(); ++b) {
							if (hits[b] != 0) {
								work.rejected[b / 32] |= 1u << (b % 32);
							}
						}
						if (_desc.validateCpu) {
							Validate(work);
						}
					}
#endif
					if (!work.error.empty()) {
						throw std::runtime_error(work.error);
					}
					if (_usesRoles) {
						ApplyRoleDecisions(work);
					}
					ApplyGrassFilters(work);
					_rejected.fetch_add(CountBits(work.rejected));
					_blades.fetch_add(work.candidates.blades.size());
					const auto finalized = Grass::FinalizeCell(work.candidates, work.rejected, _desc.blockLayout);
					const auto& cache = finalized.cache;
					if (finalized.bladesCapped != 0) {
						_capped.fetch_add(finalized.bladesCapped);
					}
					const auto names = _desc.fileSuffixes.size();
					if (_desc.skipEmpty && cache.groups.empty()) {
						// A cache left from an earlier run would otherwise still place grass here.
						// Under MO2's virtual filesystem an existing file can be another mod's (a downloaded cache):
						// a remove there deletes it inside that mod, so leave it and say so once.
						static const bool underMo2 = Platform::ModOrganizerDirectory().has_value();
						if (_desc.overwrite && underMo2) {
							static std::once_flag warned;
							std::call_once(warned, [] { spdlog::warn("not removing stale caches of now-empty cells under Mod Organizer 2: they may belong to other mods"); });
						} else if (_desc.overwrite) {
							for (std::size_t name = 0; name < names; ++name) {
								const auto path = CellPath(a_input.cell, name);
								if (!_desc.existingFiles || _desc.existingFiles->contains(Platform::LowerAscii(path.filename().string()))) {
									std::error_code error;
									if (std::filesystem::remove(path, error)) {
										spdlog::info("removed stale cache {}", path.filename().string());
									}
								}
							}
						}
						_empty.fetch_add(1);
						Finish(work);
						return done;
					}
					auto bytes = Grass::SerializeNgioCellCache(cache);
					for (std::size_t name = 0; name < names; ++name) {
						if (_desc.writer) {
							_desc.writer->Submit(CellPath(a_input.cell, name), name + 1 == names ? std::move(bytes) : bytes, _desc.writeTally);
						} else {
							Platform::WriteWholeFile(CellPath(a_input.cell, name), bytes);
						}
					}
					_written.fetch_add(names);
				} catch (const std::exception& e) {
					spdlog::error("cell {}: {}", CellPath(a_input.cell).filename().string(), e.what());
					_failed.fetch_add(1);
				}
				Finish(work);
				return done;
			}

			// The blades' volume hits and, for those touching a cliff, the cliff rays, on the CPU BVH.
			void TraceRolesOnCpu(CellWork& a_work) const
			{
				const auto& candidates = a_work.candidates;
				const auto& bvh = *_desc.cpuBvh;
				a_work.volumeHits.resize(candidates.blades.size());
				for (std::size_t b = 0; b < candidates.blades.size(); ++b) {
					const auto& blade = candidates.blades[b];
					const auto& shape = a_work.shapes[blade.groupIndex];
					const auto [p, q] = Rejection::BladeSegment(shape, blade.position);
					a_work.volumeHits[b] = bvh.ClassifyCapsule(p, q, shape.radius);
					if (Rejection::NeedsCliffRays(a_work.volumeHits[b], *_desc.features)) {
						a_work.cliffCandidates.push_back(static_cast<std::uint32_t>(b));
					}
				}
				const auto segmentHits = [&](const Float3& a_p, const Float3& a_q, std::vector<Rejection::WorldSegmentHit>& a_hits) { bvh.SegmentHitsWorld(a_p, a_q, a_hits); };
				a_work.cliffRays.reserve(a_work.cliffCandidates.size());
				for (const auto b : a_work.cliffCandidates) {
					const auto& blade = candidates.blades[b];
					a_work.cliffRays.push_back(Rejection::TraceCliffRays(segmentHits, *_desc.world, blade.position,
						Rejection::CliffNeighbourOffset(*candidates.groups[blade.groupIndex].grass)));
				}
			}

			// Turns the volume hits and cliff rays into reject bits and moved blades (and, with
			// --validate-cpu, compares each decision with the brute-force reference's).
			void ApplyRoleDecisions(CellWork& a_work)
			{
				auto& candidates = a_work.candidates;
				if (a_work.volumeHits.size() != candidates.blades.size()) {
					return;
				}
				a_work.rejected.assign((candidates.blades.size() + 31) / 32, 0u);
				const Rejection::CliffRays noRays{};
				std::size_t nextCandidate = 0;
				std::vector<Rejection::BladeDecision> decisions(candidates.blades.size());
				for (std::size_t b = 0; b < candidates.blades.size(); ++b) {
					const auto& blade = candidates.blades[b];
					const auto* rays = std::addressof(noRays);
					if (nextCandidate < a_work.cliffCandidates.size() && a_work.cliffCandidates[nextCandidate] == b) {
						rays = std::addressof(a_work.cliffRays[nextCandidate++]);
					}
					decisions[b] = Rejection::DecideBlade(a_work.volumeHits[b], *rays, *_desc.world, *_desc.features, *candidates.groups[blade.groupIndex].grass, blade.position);
				}
				if (_desc.validateCpu) {
					ValidateRoles(a_work, decisions);
				}
				std::uint64_t moved = 0;
				for (std::size_t b = 0; b < candidates.blades.size(); ++b) {
					const auto& decision = decisions[b];
					if (decision.rejected) {
						a_work.rejected[b / 32] |= 1u << (b % 32);
					} else if (decision.moved) {
						const float normal[3]{ decision.normal.x, decision.normal.y, decision.normal.z };
						Grass::MoveBlade(candidates.blades[b], candidates, decision.z, normal, _desc.placement.globalScale);
						++moved;
					}
				}
				_moved.fetch_add(moved);
			}

			void ValidateRoles(const CellWork& a_work, const std::vector<Rejection::BladeDecision>& a_decisions)
			{
				const auto& candidates = a_work.candidates;
				const auto& world = *_desc.world;
				const auto segmentHits = [&](const Float3& a_p, const Float3& a_q, std::vector<Rejection::WorldSegmentHit>& a_hits) {
					Rejection::SegmentHitsBruteForce(world, a_p, a_q, a_hits);
				};
				for (std::size_t b = 0; b < candidates.blades.size(); ++b) {
					const auto& blade = candidates.blades[b];
					const auto& shape = a_work.shapes[blade.groupIndex];
					const auto& grass = *candidates.groups[blade.groupIndex].grass;
					const auto [p, q] = Rejection::BladeSegment(shape, blade.position);
					const auto hits = Rejection::ClassifyCapsuleBruteForce(world, candidates.cellX, candidates.cellY, p, q, shape.radius);
					Rejection::CliffRays rays;
					if (Rejection::NeedsCliffRays(hits, *_desc.features)) {
						rays = Rejection::TraceCliffRays(segmentHits, world, blade.position, Rejection::CliffNeighbourOffset(grass));
					}
					const auto reference = Rejection::DecideBlade(hits, rays, world, *_desc.features, grass, blade.position);
					const auto& decision = a_decisions[b];
					const bool same = reference.rejected == decision.rejected && reference.moved == decision.moved &&
					                  (!reference.moved || std::abs(reference.z - decision.z) <= 0.5f);
					if (same) {
						continue;
					}
					if (_mismatches.fetch_add(1) < 20) {
						spdlog::warn("mismatch cell ({}, {}) blade {} at ({:.1f}, {:.1f}, {:.1f}): reference {} {}, {} {}", candidates.cellX, candidates.cellY, b,
							blade.position[0], blade.position[1], blade.position[2], reference.rejected ? "rejects" : (reference.moved ? "moves" : "keeps"), reference.z,
							_desc.backend == RejectionBackend::Gpu ? "GPU" : "CPU BVH", decision.rejected ? "rejects" : (decision.moved ? "moves" : "keeps"));
					}
				}
			}

			// NGIO checks these before its ray cast; applying them to the collision result is the same.
			void ApplyGrassFilters(CellWork& a_work) const
			{
				if (_desc.backend == RejectionBackend::None || (!_desc.ignoredGrass && !_desc.textureMask)) {
					return;
				}
				const auto& candidates = a_work.candidates;
				a_work.rejected.resize((candidates.blades.size() + 31) / 32, 0u);
				std::vector<std::uint8_t> ignoredGroup(candidates.groups.size(), 0);
				if (_desc.ignoredGrass) {
					for (std::size_t g = 0; g < candidates.groups.size(); ++g) {
						ignoredGroup[g] = _desc.ignoredGrass->contains(candidates.groups[g].grass->formID) ? 1 : 0;
					}
				}
				const auto width = _desc.textureWidth;
				for (std::size_t b = 0; b < candidates.blades.size(); ++b) {
					const auto& blade = candidates.blades[b];
					const auto bit = 1u << (b % 32);
					if (ignoredGroup[blade.groupIndex]) {
						a_work.rejected[b / 32] &= ~bit;
						continue;
					}
					if (!_desc.textureMask || (a_work.rejected[b / 32] & bit) != 0) {
						continue;
					}
					const auto x = blade.position[0];
					const auto y = blade.position[1];
					const auto& mask = *_desc.textureMask;
					bool listed = mask.Contains(x, y);
					if (!listed && width > 0.0f) {
						listed = mask.Contains(x + width, y) || mask.Contains(x - width, y) || mask.Contains(x, y + width) || mask.Contains(x, y - width);
					}
					if (listed) {
						a_work.rejected[b / 32] |= bit;
					}
				}
			}

			void Validate(const CellWork& a_work)
			{
				const auto reference = Rejection::RejectCellOnCpu(*_desc.world, a_work.candidates, a_work.shapes);
				for (std::size_t w = 0; w < reference.size(); ++w) {
					const auto diff = reference[w] ^ a_work.rejected[w];
					if (diff == 0) {
						continue;
					}
					const auto previous = _mismatches.fetch_add(static_cast<std::uint64_t>(std::popcount(diff)));
					if (previous < 20) {
						const auto bit = static_cast<std::uint32_t>(std::countr_zero(diff));
						const auto& blade = a_work.candidates.blades[w * 32 + bit];
						const auto& shape = a_work.shapes[blade.groupIndex];
						const auto [p, q] = Rejection::BladeSegment(shape, blade.position);
						// Queries are in blade order.
						const auto query = w * 32 + bit;
						spdlog::warn("mismatch cell ({}, {}) query {} blade at ({:.1f}, {:.1f}, {:.1f}) r={:.2f}: reference={} {}={} {}",
							a_work.candidates.cellX,
							a_work.candidates.cellY,
							query,
							blade.position[0],
							blade.position[1],
							blade.position[2],
							shape.radius,
							(reference[w] >> bit) & 1u,
							_desc.backend == RejectionBackend::Gpu ? "gpu" : "bvh",
							(a_work.rejected[w] >> bit) & 1u,
							Rejection::ExplainCapsule(*_desc.world, a_work.candidates.cellX, a_work.candidates.cellY, p, q, shape.radius));
					}
				}
			}

			const CellPipelineDesc& _desc;
			Graph _graph;
			CapacityBroker _capacity;
			std::function<void(std::uint64_t)> _writeNotify;
			// The world has grass cliffs or objects with ignored shapes: blades are judged by role.
			bool _usesRoles{ false };
			std::atomic<std::uint32_t> _finished{ 0 };
			std::atomic<std::uint64_t> _written{ 0 };
			std::atomic<std::uint64_t> _skipped{ 0 };
			std::atomic<std::uint64_t> _empty{ 0 };
			std::atomic<std::uint64_t> _failed{ 0 };
			std::atomic<std::uint64_t> _cancelled{ 0 };
			std::atomic<std::uint64_t> _blades{ 0 };
			std::atomic<std::uint64_t> _rejected{ 0 };
			std::atomic<std::uint64_t> _mismatches{ 0 };
			std::atomic<std::uint64_t> _moved{ 0 };
			std::atomic<std::uint64_t> _capped{ 0 };
		};
	}

	CellPipelineStats RunCellPipeline(const CellPipelineDesc& a_desc)
	{
		CellPipelineRun pipeline(a_desc);
		return pipeline.Run();
	}
}
