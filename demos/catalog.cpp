#include "demos/catalog.h"

#include <algorithm>

namespace demo {
namespace {

const WorkloadExpectation kDenseAluExpectations[] = {
    {"cycles", "low", "The loop stays in registers and avoids long-latency miss penalties, so total cycle count stays comparatively low."},
    {"instructions", "approx", "The current clang build emits about 14 loop-body instructions per iteration, so 20,000,000 iterations predict about 280,000,000 retired instructions.", ExpectationKind::ApproximateValue, 280'000'000.0, 0.01},
    {"inst-all", "approx", "The PMU all-instruction view should track the same current codegen estimate: about 14 loop-body instructions per iteration across 20,000,000 iterations.", ExpectationKind::ApproximateValue, 280'000'000.0, 0.01},
    {"inst-int-alu", "approx", "On the current build, about 13 of the 14 loop-body instructions are integer ALU-class instructions, so this counter should land near 260,000,000.", ExpectationKind::ApproximateValue, 260'000'000.0, 0.01},
    {"branches", "approx", "There is one loop-closing branch per iteration, so 20,000,000 iterations predict about 20,000,000 retired branches.", ExpectationKind::ApproximateValue, 20'000'000.0, 0.01},
    {"interrupt-pending", "low", "There are no deliberately injected asynchronous events here, so pending-interrupt pressure should stay near baseline."},
};

const WorkloadExpectation kHotReadExpectations[] = {
    {"cycles", "low", "The working set fits in L1D, so the core avoids miss-driven stalls."},
    {"l1-load-miss", "low", "Dependent loads stay resident in L1D instead of falling through to lower levels."},
    {"dtlb-miss", "low", "The address footprint is tiny, so DTLB pressure stays near zero."},
    {"l2-tlb-miss", "low", "A tiny hot footprint should not walk into second-level data TLB misses."},
    {"dtlb-miss-nonspec", "low", "The same hot footprint also keeps the non-speculative DTLB miss counter near zero."},
};

const WorkloadExpectation kScalarStreamReadExpectations[] = {
    {"inst-int-ld", "high", "This loop intentionally disables vectorization and issues one scalar load per element."},
    {"inst-simd-ld", "low", "There are no explicit SIMD loads in the scalar version."},
    {"ld-nt-uop", "low", "The baseline stream uses ordinary temporal loads rather than the non-temporal load path."},
};

const WorkloadExpectation kNtStreamReadExpectations[] = {
    {"ld-nt-uop", "high", "This stream explicitly requests non-temporal loads, so the non-temporal load-uop counter is the headline signal."},
    {"inst-int-ld", "high", "The code is still scalar and load-heavy, so scalar load retirement remains visible too."},
};

const WorkloadExpectation kSimdStreamReadExpectations[] = {
    {"inst-simd-ld", "high", "The loop uses explicit NEON vector loads, so SIMD load instructions should dominate."},
    {"inst-int-ld", "low", "The data stream is the same, but the vectorized version should retire far fewer scalar load instructions."},
};

const WorkloadExpectation kRandomPointerExpectations[] = {
    {"cycles", "high", "Each dependent load waits on the previous random miss, stretching cycle count sharply."},
    {"instructions", "low", "The loop body is instruction-light; most time is latency, not retire bandwidth."},
    {"l1-load-miss", "high", "Random dependent loads defeat prefetching and force frequent L1D misses."},
};

const WorkloadExpectation kLinearPointerExpectations[] = {
    {"cycles", "low", "The address stream is linear enough for the hardware prefetchers to stay useful, so the same number of dependent loads costs fewer cycles."},
    {"instructions", "low", "The loop body is still tiny; this is a latency baseline, not an instruction-density demo."},
    {"l1-load-miss", "low", "A linear walk keeps spatial locality intact, so miss pressure drops sharply versus the random chase."},
};

const WorkloadExpectation kHotWriteExpectations[] = {
    {"l1-store-miss", "low", "Repeated stores revisit the same hot cache lines, so store misses stay low."},
};

const WorkloadExpectation kScalarStreamWriteExpectations[] = {
    {"inst-int-st", "high", "This loop intentionally disables vectorization and performs scalar stores through the whole stream."},
    {"inst-simd-st", "low", "There are no explicit SIMD stores in the scalar version."},
    {"st-nt-uop", "low", "The baseline stream uses ordinary temporal stores rather than the non-temporal store path."},
};

const WorkloadExpectation kNtStreamWriteExpectations[] = {
    {"st-nt-uop", "high", "Explicit stnp pair stores light this up -- but on M4 P-cores so do ordinary scalar stores, so st-nt-uop just tracks scalar store uops (about equal to inst-int-st here) and does not isolate the non-temporal path; see docs/LIMITATIONS.md."},
    {"inst-int-st", "high", "The body is still scalar and store-heavy, so scalar store retirement should remain visible."},
};

const WorkloadExpectation kSimdStreamWriteExpectations[] = {
    {"inst-simd-st", "high", "The loop uses explicit NEON stores, so SIMD store instructions should dominate."},
    {"inst-int-st", "low", "The vectorized store path should retire far fewer scalar store instructions."},
};

const WorkloadExpectation kRandomWriteExpectations[] = {
    {"l1-store-miss", "high", "One store per widely scattered page pushes writes onto cold lines."},
    {"dtlb-miss", "high", "Random page order churns the DTLB while issuing those stores."},
};

const WorkloadExpectation kUncontendedAtomicExpectations[] = {
    {"atomic-succ", "approx", "The loop performs 4,000,000 compare-exchange updates, and without contention nearly every one should retire as a successful atomic operation.", ExpectationKind::ApproximateValue, 4'000'000.0, 0.01},
    {"atomic-fail", "near-zero", "With no competing writers, the retry path should stay at or extremely close to zero.", ExpectationKind::NearZero, 1'000.0, 0.0},
};

const WorkloadExpectation kContendedAtomicExpectations[] = {
    {"atomic-succ", "approx", "The measured thread still executes 2,000,000 compare-exchange updates, so successful atomics should stay close to that exact loop trip count even under contention.", ExpectationKind::ApproximateValue, 2'000'000.0, 0.01},
    {"atomic-fail", "high", "Competing helper threads keep invalidating the expected value, so failed atomic attempts rise sharply."},
};

const WorkloadExpectation kBarrierExpectations[] = {
    {"inst-barrier", "approx", "The loop executes one real memory barrier per iteration, so 20,000,000 iterations predict about 20,000,000 retired barrier instructions.", ExpectationKind::ApproximateValue, 20'000'000.0, 0.01},
};

const WorkloadExpectation kInterruptStormExpectations[] = {
    {"cycles", "high", "A helper thread floods the measured thread with signals; each delivery forces a kernel round trip, so total cycles balloon several-fold versus the same arithmetic run uninterrupted. (The dedicated interrupt-pending PMC does not respond to software signals on M4 and stays at noise -- watch cycles, not that event.)"},
};

const WorkloadExpectation kPageStrideExpectations[] = {
    {"dtlb-miss", "high", "One demand access per shuffled page maximizes translation pressure."},
    {"l2-tlb-miss", "high", "Enough distinct pages overflow the first-level data TLB and spill into the next level."},
    {"dtlb-miss-nonspec", "high", "The same page-scattered access pattern also keeps the non-speculative miss view elevated."},
    {"dtlb-access", "high", "Every sparse page access has to consult the translation machinery."},
    {"dtlb-fill", "high", "The hot DTLB entries get churned constantly, so fills rise alongside misses."},
    {"mmu-table-walk-data", "high", "This is one of the clearest data-side page-walk triggers in the lab."},
};

const WorkloadExpectation kFirstTouchFaultExpectations[] = {
    {"dtlb-miss", "high", "Those cold first touches also churn translation state because every page is newly visited."},
    {"mmu-table-walk-data", "high", "The first-touch walk forces real data-side translation work while the mapping is populated."},
};

const WorkloadExpectation kAlignedX64Expectations[] = {
    {"ldst-x64-uop", "low", "An aligned 64-bit load at the start of each 64-byte region should not need a split-64B micro-op."},
    {"ldst-xpg-uop", "low", "The access stays fully inside both the 64-byte region and the page."},
};

const WorkloadExpectation kCrossX64Expectations[] = {
    {"ldst-x64-uop", "high", "Starting each 64-bit load at byte 60 forces it to straddle a 64-byte boundary."},
    {"ldst-xpg-uop", "low", "Most accesses still stay inside a page; this workload is about 64-byte splits, not page splits."},
};

const WorkloadExpectation kAlignedPageExpectations[] = {
    {"ldst-xpg-uop", "low", "A load from the start of each page stays completely inside that page."},
    {"ldst-x64-uop", "low", "The aligned page-start access also avoids 64-byte splits."},
};

const WorkloadExpectation kCrossPageExpectations[] = {
    {"ldst-xpg-uop", "high", "Starting a 64-bit load four bytes before the page boundary forces a true cross-page access."},
    {"ldst-x64-uop", "high", "That same offset also straddles a 64-byte region, so both split counters should rise together."},
};

const WorkloadExpectation kAlignedX64StoreExpectations[] = {
    {"ldst-x64-uop", "low", "An aligned 64-bit store at the start of each 64-byte region should not need a split-64B micro-op."},
    {"ldst-xpg-uop", "low", "The store stays fully inside both the 64-byte region and the page."},
};

const WorkloadExpectation kCrossX64StoreExpectations[] = {
    {"ldst-x64-uop", "high", "Starting each 64-bit store at byte 60 forces it to straddle a 64-byte boundary."},
    {"ldst-xpg-uop", "low", "The store stream is still page-local; this demo is about 64-byte splits, not page splits."},
};

const WorkloadExpectation kAlignedPageStoreExpectations[] = {
    {"ldst-xpg-uop", "low", "A store to the start of each page stays completely inside that page."},
    {"ldst-x64-uop", "low", "The aligned page-start store also avoids 64-byte splits."},
};

const WorkloadExpectation kCrossPageStoreExpectations[] = {
    {"ldst-xpg-uop", "high", "Starting a 64-bit store four bytes before the page boundary forces a true cross-page store."},
    {"ldst-x64-uop", "high", "That same store also straddles a 64-byte region, so both split counters should rise together."},
};

const WorkloadExpectation kSimdAluExpectations[] = {
    {"inst-simd-alu", "high", "The loop body is explicit NEON arithmetic on vectors, so SIMD ALU retirement should be the headline counter."},
};

const WorkloadExpectation kDispatchIntExpectations[] = {
    {"core-active-cycle", "high", "The core spends most of its time actively issuing useful work instead of waiting on misses."},
    {"retire-uop", "high", "A dense scalar compute loop should retire a large stream of micro-ops."},
    {"map-uop", "high", "The mapper keeps feeding uops from a steady stream of scalar integer operations."},
    {"map-int-uop", "high", "Most of those mapped uops are scalar integer work rather than loads or SIMD instructions."},
};

const WorkloadExpectation kDispatchMemoryExpectations[] = {
    {"retire-uop", "high", "A long streaming loop still retires a large body of micro-ops, even though the mix is load-heavy."},
    {"map-uop", "high", "The mapper stays busy issuing uops for the steady-state streaming loop."},
    {"map-ldst-uop", "high", "The dominant mapped work is load/store traffic rather than scalar ALU or SIMD arithmetic."},
};

const WorkloadExpectation kDispatchSimdExpectations[] = {
    {"core-active-cycle", "high", "The vector loop stays register-resident and keeps the core actively working."},
    {"retire-uop", "high", "A dense SIMD compute loop should still retire a large stream of uops."},
    {"map-uop", "high", "The mapper keeps feeding uops from a dense vector arithmetic loop."},
    {"map-simd-uop", "high", "Most of those mapped uops are SIMD arithmetic rather than scalar integer work."},
};

const WorkloadExpectation kFrontendHotRestartExpectations[] = {
    {"fetch-restart", "low", "One tiny hot stub should let the frontend fetch smoothly without frequent restart events."},
    {"flush-restart-other", "low", "The hot single-page code path should minimize non-branch frontend flush/restart activity."},
    {"map-dispatch-bubble", "low", "One tiny hot stub is the low-bubble frontend baseline: fetch and dispatch should stay smooth."},
    {"map-stall", "low", "This is the low-stall mapper baseline because the frontend stays hot and branch targets stay fixed."},
    {"map-stall-dispatch", "low", "With one hot stub, dispatch-facing mapper stalls should stay comparatively rare."},
};

const WorkloadExpectation kFrontendRandomRestartExpectations[] = {
    {"fetch-restart", "high", "Jumping across many executable pages makes the frontend repeatedly rediscover and restart fetch from new locations, so fetch-restart is the headline signal."},
};

const WorkloadExpectation kFrontendSelfModifyingExpectations[] = {
    {"flush-restart-other", "high", "The workload rewrites executable code, invalidates the I-cache, and re-enters execution repeatedly, so non-branch frontend flush/restart activity is the headline signal."},
    {"fetch-restart", "high", "Patching and re-entering alternating stubs also forces the frontend to rediscover code streams repeatedly, so fetch restart is a supporting signal."},
};

const WorkloadExpectation kPredictableBranchExpectations[] = {
    {"branches", "high", "The loop executes an actual conditional branch every iteration, so retired branch count is high."},
    {"inst-branch-cond", "high", "Every iteration still uses a conditional branch, even though the predictor handles it well."},
    {"inst-branch-taken", "high", "The branch is taken almost every iteration in the biased pattern, so taken-branch count rises strongly."},
    {"branch-miss", "low", "The condition is strongly biased, so the predictor stays accurate."},
    {"branch-cond-miss", "low", "The conditional-branch-specific mispredict counter should stay low on the biased pattern."},
    {"map-rewind", "low", "Because the predictor stays accurate, the mapper should need far fewer rewinds than in the random-direction branch case."},
};

const WorkloadExpectation kUnpredictableBranchExpectations[] = {
    {"branches", "high", "The loop still executes a real conditional branch every iteration."},
    {"inst-branch-cond", "high", "Conditional branch count stays high because the loop shape is still branch-heavy."},
    {"branch-miss", "high", "Random branch direction prevents the predictor from settling on an accurate history."},
    {"branch-cond-miss", "high", "This is the conditional-branch-specific version of the same misprediction story."},
    {"map-rewind", "high", "Frequent branch redirections should force the mapper to throw away more speculative work and rewind more often."},
};

const WorkloadExpectation kHotInstructionExpectations[] = {
    {"cycles", "low", "Calling the same tiny stub repeatedly keeps the frontend hot."},
    {"inst-branch-call", "high", "Every iteration makes a real function call into the stub."},
    {"inst-branch-ret", "high", "Each stub invocation returns immediately, so return-branch count rises too."},
    {"inst-branch-indir", "high", "The function pointer call is an indirect branch every iteration."},
    {"itlb-miss", "low", "One hot code page should not create ITLB pressure."},
    {"l1i-tlb-fill", "low", "With one hot code page there is very little need to refill the instruction-side TLB."},
    {"l1i-cache-miss", "low", "The same stub stays resident in the instruction cache."},
    {"l2-tlb-miss-instruction", "low", "A single hot code page should not spill into second-level instruction-side TLB misses."},
    {"mmu-table-walk-instruction", "low", "There is almost no instruction-side page walking once the hot page is installed."},
    {"branch-call-indir-miss", "low", "The indirect call target never changes, so the indirect-call predictor should settle quickly."},
    {"branch-indir-miss", "low", "A stable function-pointer target is the low-mispredict baseline for indirect branches."},
    {"branch-ret-indir-miss", "low", "The same tiny stub returns to the same site every time, which is friendly to return prediction."},
    {"map-dispatch-bubble-ic", "low", "One hot code page minimizes instruction-cache-driven dispatch bubbles."},
    {"map-dispatch-bubble-itlb", "low", "One hot code page also minimizes ITLB-driven dispatch bubbles."},
};

const WorkloadExpectation kRandomInstructionExpectations[] = {
    {"inst-branch-call", "high", "The workload is still a stream of indirect calls, one per visited code page."},
    {"inst-branch-ret", "high", "Every visited stub also returns, so return count remains high."},
    {"inst-branch-indir", "high", "The changing function-pointer target makes indirect branch activity obvious."},
    {"itlb-miss", "high", "Jumping across many executable pages churns the ITLB."},
    {"l1i-tlb-fill", "high", "The instruction-side TLB needs constant refills as code-page locality disappears."},
    {"l1i-cache-miss", "high", "Randomized code-page order also defeats L1I locality."},
    {"l2-tlb-miss-instruction", "high", "Enough code pages spill beyond the first-level ITLB and show up in the next level too."},
    {"mmu-table-walk-instruction", "high", "The frontend has to perform real instruction-side page walks as it rediscovers code pages."},
    {"branch-call-indir-miss", "high", "The indirect call target keeps changing across many stubs, which is the cleanest indirect-call mispredict trigger in the current lab."},
    {"branch-indir-miss", "high", "This is also a changing indirect-branch target stream, so general indirect-branch mispredicts should rise too."},
    {"branch-ret-indir-miss", "high", "The call/return stream jumps across many indirect targets, which is the strongest return-side indirect-branch stress case we currently have."},
    {"map-dispatch-bubble-ic", "high", "Randomized code-page execution is exactly the kind of instruction-cache churn that should show up as dispatch bubbles."},
    {"map-dispatch-bubble-itlb", "high", "The same code-page churn also keeps the instruction-side translation path busy, making ITLB-related dispatch bubbles the intended high case."},
};

const WorkloadExpectation kStoreOrderFriendlyExpectations[] = {
    {"cycles", "low", "The younger load always targets a slot disjoint from the store, so the load/store unit never stalls or replays; this is the low-cycle baseline."},
};

const WorkloadExpectation kStoreOrderAliasExpectations[] = {
    {"cycles", "high", "Unpredictable store/load aliasing forces the load/store unit to stall and replay speculatively-issued loads, inflating cycles ~1.6x over the disjoint baseline even though the instruction stream is byte-for-byte identical."},
    {"st-memory-order-violation", "near-zero", "The squash-and-replay penalty is real (it shows up in cycles), but Apple's M4 P-cores leave this dedicated ordering-violation event at zero -- the cost is paid yet not surfaced by this PMC.", ExpectationKind::NearZero, 1'000.0, 0.0},
};

constexpr std::string_view kDenseAluConfig =
    "20,000,000 iterations over four 64-bit registers. No deliberate data working set.";
constexpr std::string_view kDenseAluCode = R"cpp(
std::uint64_t a = 0x12345678ULL;
for (std::size_t i = 0; i < 20'000'000; ++i) {
  a += (b ^ i) + 0x9e3779b97f4a7c15ULL;
  b = (b << 7) | (b >> 57);
  b ^= a + c;
  c += (a & 0xffffULL) * 17ULL + d;
  d ^= c + (a >> 3);
}
)cpp";

constexpr std::string_view kHotReadConfig =
    "32,000,000 dependent loads through a 32 KiB sequential ring.";
constexpr std::string_view kHotReadCode = R"cpp(
volatile const std::uint32_t *ring = state.hot_ring.data();
std::uint32_t index = 0;
for (std::size_t i = 0; i < 32'000'000; ++i) {
  index = ring[index];
}
)cpp";

constexpr std::string_view kScalarStreamReadConfig =
    "8 passes over a 64 MiB uint64_t array with vectorization disabled.";
constexpr std::string_view kScalarStreamReadCode = R"cpp(
#pragma clang loop vectorize(disable)
for (std::size_t i = 0; i < state.stream_read.size(); ++i) {
  sum += data[i];
}
)cpp";

constexpr std::string_view kNtStreamReadConfig =
    "8 passes over the same 64 MiB stream, but using explicit non-temporal pair loads.";
constexpr std::string_view kNtStreamReadCode = R"cpp(
#pragma clang loop vectorize(disable)
for (std::size_t i = 0; i + 1 < state.stream_read.size(); i += 2) {
  asm volatile("ldnp %x0, %x1, [%2]" : "=&r"(a), "=&r"(b) : "r"(data + i) : "memory");
  sum0 += a;
  sum1 += b;
}
)cpp";

constexpr std::string_view kSimdStreamReadConfig =
    "8 passes over the same 64 MiB stream, but using explicit NEON 128-bit loads.";
constexpr std::string_view kSimdStreamReadCode = R"cpp(
for (std::size_t i = 0; i < state.stream_read.size(); i += 2) {
  acc = vaddq_u64(acc, vld1q_u64(data + i));
}
)cpp";

constexpr std::string_view kRandomPointerConfig =
    "2,000,000 dependent loads through a 32 MiB randomized ring.";
constexpr std::string_view kRandomPointerCode = R"cpp(
volatile const std::uint32_t *ring = state.random_ring.data();
std::uint32_t index = 0;
for (std::size_t i = 0; i < 2'000'000; ++i) {
  index = ring[index];
}
)cpp";

constexpr std::string_view kLinearPointerConfig =
    "2,000,000 dependent loads through a 32 MiB linear ring.";
constexpr std::string_view kLinearPointerCode = R"cpp(
volatile const std::uint32_t *ring = state.linear_ring.data();
std::uint32_t index = 0;
for (std::size_t i = 0; i < 2'000'000; ++i) {
  index = ring[index];
}
)cpp";

constexpr std::string_view kHotWriteConfig =
    "8,192 passes over a 32 KiB hot write array.";
constexpr std::string_view kHotWriteCode = R"cpp(
for (std::size_t pass = 0; pass < 8192; ++pass) {
  for (std::size_t i = 0; i < state.hot_store.size(); ++i) {
    data[i] += static_cast<std::uint64_t>(i + pass);
  }
}
)cpp";

constexpr std::string_view kScalarStreamWriteConfig =
    "8 passes over a 64 MiB uint64_t array with vectorization disabled.";
constexpr std::string_view kScalarStreamWriteCode = R"cpp(
#pragma clang loop vectorize(disable)
for (std::size_t i = 0; i < state.stream_store.size(); ++i) {
  data[i] += static_cast<std::uint64_t>(i + pass + 1);
}
)cpp";

constexpr std::string_view kNtStreamWriteConfig =
    "8 passes over the same 64 MiB stream, but using explicit non-temporal pair stores.";
constexpr std::string_view kNtStreamWriteCode = R"cpp(
#pragma clang loop vectorize(disable)
for (std::size_t i = 0; i + 1 < state.stream_store.size(); i += 2) {
  const std::uint64_t value0 = seed ^ (0x100000001b3ULL * (i + 1));
  const std::uint64_t value1 = (seed + 0x9e3779b97f4a7c15ULL) ^ (0xc2b2ae3d27d4eb4fULL * (i + 3));
  asm volatile("stnp %x0, %x1, [%2]" : : "r"(value0), "r"(value1), "r"(data + i) : "memory");
}
)cpp";

constexpr std::string_view kSimdStreamWriteConfig =
    "8 passes over the same 64 MiB stream, but using explicit NEON 128-bit stores.";
constexpr std::string_view kSimdStreamWriteCode = R"cpp(
for (std::size_t i = 0; i < state.stream_store.size(); i += 2) {
  const uint64x2_t value = vaddq_u64(vld1q_u64(data + i), bias);
  vst1q_u64(data + i, value);
}
)cpp";

constexpr std::string_view kRandomWriteConfig =
    "256 passes; one store per shuffled page across a 64 MiB footprint.";
constexpr std::string_view kRandomWriteCode = R"cpp(
for (std::size_t pass = 0; pass < 256; ++pass) {
  for (std::size_t page : state.page_order) {
    base[page * stride] += static_cast<std::uint64_t>(pass + 1);
  }
}
)cpp";

constexpr std::string_view kUncontendedAtomicConfig =
    "4,000,000 compare-exchange increments on one private atomic word.";
constexpr std::string_view kUncontendedAtomicCode = R"cpp(
for (std::size_t i = 0; i < 4'000'000; ++i) {
  std::uint64_t expected = value.load(std::memory_order_relaxed);
  while (!value.compare_exchange_weak(expected, expected + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
  }
}
)cpp";

constexpr std::string_view kContendedAtomicConfig =
    "2,000,000 compare-exchange increments from the measured thread while three helpers race on the same atomic word.";
constexpr std::string_view kContendedAtomicCode = R"cpp(
std::array<std::thread, 3> helpers = { ... };
start.store(true, std::memory_order_release);
for (std::size_t i = 0; i < 2'000'000; ++i) {
  std::uint64_t expected = value.load(std::memory_order_relaxed);
  while (!value.compare_exchange_weak(expected, expected + 1,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
  }
}
)cpp";

constexpr std::string_view kBarrierLoopConfig =
    "20,000,000 arithmetic iterations with an explicit full memory barrier in each one.";
constexpr std::string_view kBarrierLoopCode = R"cpp(
for (std::size_t i = 0; i < 20'000'000; ++i) {
  a += (b ^ i) + 0x9e3779b97f4a7c15ULL;
  asm volatile("dmb ish" ::: "memory");
  b ^= a + (a >> 7);
}
)cpp";

constexpr std::string_view kInterruptStormConfig =
    "50,000,000 arithmetic iterations while a helper thread floods the measured thread with up to 100,000 SIGUSR1 deliveries.";
constexpr std::string_view kInterruptStormCode = R"cpp(
std::thread sender([&] {
  for (std::size_t i = 0; i < 100'000; ++i) {
    pthread_kill(target_thread, SIGUSR1);   // preempt the measured thread
  }
});
for (std::size_t i = 0; i < 50'000'000; ++i) {
  a += (b ^ i) + 0x9e3779b97f4a7c15ULL;
  b = (b << 9) | (b >> 55);
  b ^= a + 0x85ebca6bULL;
}
)cpp";

constexpr std::string_view kPageStrideConfig =
    "256 passes; one load per shuffled page across a 64 MiB footprint.";
constexpr std::string_view kPageStrideCode = R"cpp(
std::uint64_t sum = 0;
for (std::size_t pass = 0; pass < 256; ++pass) {
  for (std::size_t page : state.page_order) {
    sum += base[page * stride];
  }
}
)cpp";

constexpr std::string_view kFirstTouchFaultConfig =
    "Create a fresh 256 MiB sparse file mapping and write one byte per page exactly once.";
constexpr std::string_view kFirstTouchFaultCode = R"cpp(
int fd = mkstemp(path);
ftruncate(fd, 256 * 1024 * 1024);
void *mapping = mmap(nullptr, 256 * 1024 * 1024, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
for (std::size_t page = 0; page < pages; ++page) {
  bytes[page * state.page_size] = static_cast<std::uint8_t>(page);
}
msync(mapping, 256 * 1024 * 1024, MS_SYNC);
munmap(mapping, 256 * 1024 * 1024);
)cpp";

constexpr std::string_view kAlignedX64Config =
    "16 passes of 64-bit loads from the start of each 64-byte region across the page-backed working set.";
constexpr std::string_view kAlignedX64Code = R"cpp(
for (std::size_t offset = 0; offset <= limit; offset += 64) {
  sum += ReadUnaligned64(bytes + offset);
}
)cpp";

constexpr std::string_view kCrossX64Config =
    "16 passes of 64-bit loads starting at byte 60 of each 64-byte region.";
constexpr std::string_view kCrossX64Code = R"cpp(
for (std::size_t offset = 0; offset <= limit; offset += 64) {
  sum += ReadUnaligned64(bytes + offset + 60);
}
)cpp";

constexpr std::string_view kAlignedPageConfig =
    "256 passes of one 64-bit load from the start of each page.";
constexpr std::string_view kAlignedPageCode = R"cpp(
for (std::size_t page = 0; page < state.page_count; ++page) {
  sum += ReadUnaligned64(bytes + page * state.page_size);
}
)cpp";

constexpr std::string_view kCrossPageConfig =
    "256 passes of one 64-bit load starting four bytes before each page boundary.";
constexpr std::string_view kCrossPageCode = R"cpp(
for (std::size_t page = 0; page < state.page_count; ++page) {
  sum += ReadUnaligned64(bytes + page * state.page_size + state.page_size - 4);
}
)cpp";

constexpr std::string_view kAlignedX64StoreConfig =
    "16 passes of 64-bit stores to the start of each 64-byte region across the page-backed working set.";
constexpr std::string_view kAlignedX64StoreCode = R"cpp(
for (std::size_t offset = 0; offset <= limit; offset += 64) {
  WriteUnaligned64(bytes + offset, stamp);
}
)cpp";

constexpr std::string_view kCrossX64StoreConfig =
    "16 passes of 64-bit stores starting at byte 60 of each 64-byte region.";
constexpr std::string_view kCrossX64StoreCode = R"cpp(
for (std::size_t offset = 0; offset <= limit; offset += 64) {
  WriteUnaligned64(bytes + offset + 60, stamp);
}
)cpp";

constexpr std::string_view kAlignedPageStoreConfig =
    "256 passes of one 64-bit store to the start of each page.";
constexpr std::string_view kAlignedPageStoreCode = R"cpp(
for (std::size_t page = 0; page < state.page_count; ++page) {
  WriteUnaligned64(bytes + page * state.page_size, stamp);
}
)cpp";

constexpr std::string_view kCrossPageStoreConfig =
    "256 passes of one 64-bit store starting four bytes before each page boundary.";
constexpr std::string_view kCrossPageStoreCode = R"cpp(
for (std::size_t page = 0; page < state.page_count; ++page) {
  WriteUnaligned64(bytes + page * state.page_size + state.page_size - 4, stamp);
}
)cpp";

constexpr std::string_view kSimdAluConfig =
    "20,000,000 iterations of register-resident NEON integer vector arithmetic.";
constexpr std::string_view kSimdAluCode = R"cpp(
for (std::size_t i = 0; i < 20'000'000; ++i) {
  a = vaddq_u64(a, b);
  b = veorq_u64(vshlq_n_u64(b, 1), c);
  c = vaddq_u64(c, vorrq_u64(a, d));
  d = veorq_u64(d, vshrq_n_u64(a, 7));
}
)cpp";

constexpr std::string_view kDispatchIntConfig =
    "Same dense scalar integer ALU loop, but measured with active-cycle and uop-mapping counters.";
constexpr std::string_view kDispatchIntCode = kDenseAluCode;

constexpr std::string_view kDispatchMemoryConfig =
    "Same scalar 64 MiB sequential read stream, but measured with active-cycle and mapper/uop counters.";
constexpr std::string_view kDispatchMemoryCode = kScalarStreamReadCode;

constexpr std::string_view kDispatchSimdConfig =
    "Same NEON vector ALU loop, but measured with active-cycle and SIMD mapper/uop counters.";
constexpr std::string_view kDispatchSimdCode = kSimdAluCode;

constexpr std::string_view kFrontendHotRestartConfig =
    "1,048,576 indirect calls to the same tiny executable stub.";
constexpr std::string_view kFrontendHotRestartCode = R"cpp(
const auto fn = state.ExecStubAt(0);
for (std::size_t i = 0; i < 1'048'576; ++i) {
  sum += fn();
}
)cpp";

constexpr std::string_view kFrontendRandomRestartConfig =
    "1,048,576 indirect calls across the shuffled executable-page set, with the same total call count as the hot baseline.";
constexpr std::string_view kFrontendRandomRestartCode = R"cpp(
for (std::size_t i = 0; i < 1'048'576; ++i) {
  const std::size_t page = state.exec_page_order[i % state.exec_page_order.size()];
  sum += state.ExecStubAt(page)();
}
)cpp";

constexpr std::string_view kFrontendSelfModifyingConfig =
    "32,768 patch blocks. Each block rewrites one of two executable stubs, invalidates its I-cache lines, then executes that stub 32 times.";
constexpr std::string_view kFrontendSelfModifyingCode = R"cpp(
for (std::size_t block = 0; block < 32'768; ++block) {
  const std::size_t target_page = block & 1u;
  mprotect(code, bytes, PROT_READ | PROT_WRITE);
  PatchExecStub(code + target_page * page_size, block + 1);
  sys_icache_invalidate(code + target_page * page_size, 8);
  mprotect(code, bytes, PROT_READ | PROT_EXEC);
  for (std::size_t i = 0; i < 32; ++i) {
    sum += reinterpret_cast<ExecStub>(code + target_page * page_size)();
  }
}
)cpp";

constexpr std::string_view kPredictableBranchConfig =
    "16 passes over 1,048,576 elements with a strongly biased taken/not-taken pattern.";
constexpr std::string_view kPredictableBranchCode = R"cpp(
for (std::size_t i = 0; i < state.branch_source.size(); ++i) {
  if ((i & 255ULL) != 0) {
    sum = BranchTakenPath(sum, i + pass);
  } else {
    sum = BranchNotTakenPath(sum, pass + 1);
  }
}
)cpp";

constexpr std::string_view kUnpredictableBranchConfig =
    "16 passes over 1,048,576 random 64-bit words; branch direction changes unpredictably.";
constexpr std::string_view kUnpredictableBranchCode = R"cpp(
const std::uint64_t bits = data[i] ^ static_cast<std::uint64_t>(pass);
if (bits & 1ULL) {
  sum = BranchTakenPath(sum, bits + i);
} else {
  sum = BranchNotTakenPath(sum, bits + pass + 1ULL);
}
)cpp";

constexpr std::string_view kStoreOrderFriendlyConfig =
    "24,000,000 store-then-load iterations over a 32 KiB L1-resident buffer; the load is always routed to a slot disjoint from the store.";
constexpr std::string_view kStoreOrderFriendlyCode = R"cpp(
r = r * 6364136223846793005ULL + 1442695040888963407ULL;
const std::size_t j = (r >> 33) & mask;   // store slot
buf[j] = i + acc;                         // older store
const std::size_t k = (j ^ 0x800) & mask; // load never aliases the store
acc += buf[k];                            // younger load
)cpp";

constexpr std::string_view kStoreOrderAliasConfig =
    "24,000,000 store-then-load iterations over a 32 KiB L1-resident buffer; ~50% of loads unpredictably alias the preceding store's slot.";
constexpr std::string_view kStoreOrderAliasCode = R"cpp(
r = r * 6364136223846793005ULL + 1442695040888963407ULL;
const std::size_t j = (r >> 33) & mask;          // store slot
buf[j] = i + acc;                                // older store
const std::size_t k = (r & 1) ? j                // ~50% alias, unpredictable
                              : (j ^ 0x800) & mask;
acc += buf[k];                                   // younger load may alias store
)cpp";

constexpr std::string_view kHotInstructionConfig =
    "32,000,000 calls to the same tiny executable stub on one hot code page.";
constexpr std::string_view kHotInstructionCode = R"cpp(
const auto fn = state.ExecStubAt(0);
for (std::size_t i = 0; i < 32'000'000; ++i) {
  sum += fn();
}
)cpp";

constexpr std::string_view kRandomInstructionConfig =
    "128 passes over many executable pages in randomized order across a 64 MiB code footprint.";
constexpr std::string_view kRandomInstructionCode = R"cpp(
for (std::size_t pass = 0; pass < 128; ++pass) {
  for (std::size_t page : state.exec_page_order) {
    sum += state.ExecStubAt(page)();
  }
}
)cpp";

const WorkloadDefinition kWorkloads[] = {
    {
        "dense-integer-alu",
        "Dense Integer ALU",
        "A hot arithmetic loop with very little memory traffic.",
        "Use this as the instruction-dense baseline: it retires lots of work without relying on the memory hierarchy or hard-to-predict branches.",
        kDenseAluConfig,
        kDenseAluCode,
        Group::CoreExecution,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | BRANCHES | PerfCounter::Named("INST_ALL") |
            PerfCounter::Named("INST_INT_ALU"),
        std::span<const WorkloadExpectation>(kDenseAluExpectations),
        &workloads::DenseIntegerAlu,
    },
    {
        "hot-seq-read",
        "Hot Sequential Read",
        "A tiny dependent load ring that stays resident in L1D.",
        "This is the low-miss memory baseline: the footprint is intentionally small enough to stay hot in L1D and the DTLB.",
        kHotReadConfig,
        kHotReadCode,
        Group::MemoryCache,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | L1_LOAD_MISS | DTLB_MISS | L2_TLB_MISS |
            PerfCounter::Named("L1D_TLB_MISS_NONSPEC"),
        std::span<const WorkloadExpectation>(kHotReadExpectations),
        &workloads::HotSequentialRead,
    },
    {
        "scalar-stream-read",
        "Scalar Stream Read",
        "A large sequential read stream compiled to stay scalar.",
        "Use this as the scalar load baseline: the data stream is simple and sequential, but the code path deliberately avoids SIMD loads.",
        kScalarStreamReadConfig,
        kScalarStreamReadCode,
        Group::MemoryCache,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_INT_LD") |
            PerfCounter::Named("INST_SIMD_LD"),
        std::span<const WorkloadExpectation>(kScalarStreamReadExpectations),
        &workloads::ScalarStreamRead,
    },
    {
        "nt-stream-read",
        "Non-Temporal Stream Read",
        "A large sequential read stream using explicit non-temporal scalar loads.",
        "This keeps the same basic streaming footprint as the scalar read baseline, but requests the non-temporal load path explicitly so the NT load-uop counter has a dedicated showcase.",
        kNtStreamReadConfig,
        kNtStreamReadCode,
        Group::MemoryCache,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LD_NT_UOP") |
            PerfCounter::Named("INST_INT_LD"),
        std::span<const WorkloadExpectation>(kNtStreamReadExpectations),
        &workloads::NtStreamRead,
        "scalar-stream-read",
        "Both demos sweep the same 64 MiB array with scalar code. The main difference is whether each load uses the ordinary temporal path or explicitly requests non-temporal behavior.",
    },
    {
        "simd-stream-read",
        "SIMD Stream Read",
        "A large sequential read stream using explicit NEON vector loads.",
        "This keeps the data pattern the same as the scalar stream read, but moves the work into SIMD load instructions.",
        kSimdStreamReadConfig,
        kSimdStreamReadCode,
        Group::MemoryCache,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_INT_LD") |
            PerfCounter::Named("INST_SIMD_LD"),
        std::span<const WorkloadExpectation>(kSimdStreamReadExpectations),
        &workloads::SimdStreamRead,
        "scalar-stream-read",
        "Both demos walk the same 64 MiB stream. The main difference is whether the loads retire as scalar integer loads or explicit NEON vector loads.",
    },
    {
        "linear-pointer-chase",
        "Linear Pointer Chase",
        "A same-size dependent load chain, but with linear address order.",
        "This is the contrast case for random pointer chasing: the dependency is still there, but the access pattern preserves spatial predictability.",
        kLinearPointerConfig,
        kLinearPointerCode,
        Group::MemoryCache,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | L1_LOAD_MISS,
        std::span<const WorkloadExpectation>(kLinearPointerExpectations),
        &workloads::LinearPointerChase,
    },
    {
        "random-pointer-chase",
        "Random Pointer Chase",
        "A large dependent load chain that defeats prefetching.",
        "Every load depends on the last one and lands in a random location, so miss latency directly turns into extra cycles.",
        kRandomPointerConfig,
        kRandomPointerCode,
        Group::MemoryCache,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | L1_LOAD_MISS,
        std::span<const WorkloadExpectation>(kRandomPointerExpectations),
        &workloads::RandomPointerChase,
        "linear-pointer-chase",
        "Compare this against the same-size linear pointer chase. The instruction count stays similar, so the wall-time and cycle gap mostly reflects lost spatial predictability and miss latency.",
    },
    {
        "hot-seq-write",
        "Hot Sequential Write",
        "A small write loop that keeps touching the same cache lines.",
        "This is the low store-miss baseline: repeated writes stay on hot lines instead of forcing constant write-allocate traffic.",
        kHotWriteConfig,
        kHotWriteCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | L1_STORE_MISS,
        std::span<const WorkloadExpectation>(kHotWriteExpectations),
        &workloads::HotSequentialWrite,
    },
    {
        "scalar-stream-write",
        "Scalar Stream Write",
        "A large sequential write stream compiled to stay scalar.",
        "Use this as the scalar store baseline: the footprint is wide, but the code path deliberately avoids SIMD stores.",
        kScalarStreamWriteConfig,
        kScalarStreamWriteCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_INT_ST") |
            PerfCounter::Named("INST_SIMD_ST"),
        std::span<const WorkloadExpectation>(kScalarStreamWriteExpectations),
        &workloads::ScalarStreamWrite,
    },
    {
        "nt-stream-write",
        "Non-Temporal Stream Write",
        "A large sequential write stream using explicit non-temporal scalar stores.",
        "This keeps the same wide store footprint as the scalar write baseline, but requests the non-temporal store path so the NT store-uop counter has a dedicated teaching case.",
        kNtStreamWriteConfig,
        kNtStreamWriteCode,
        Group::StoreOrdering,
        Tier::Experimental,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("ST_NT_UOP") |
            PerfCounter::Named("INST_INT_ST"),
        std::span<const WorkloadExpectation>(kNtStreamWriteExpectations),
        &workloads::NtStreamWrite,
        "hot-seq-write",
        "Compare this against the tiny hot-write baseline. The main difference is whether stores keep revisiting the same resident lines, or sweep a large footprint through the explicit non-temporal store path.",
    },
    {
        "simd-stream-write",
        "SIMD Stream Write",
        "A large sequential write stream using explicit NEON vector stores.",
        "This keeps the store pattern the same as the scalar stream write, but moves the work into SIMD store instructions.",
        kSimdStreamWriteConfig,
        kSimdStreamWriteCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_INT_ST") |
            PerfCounter::Named("INST_SIMD_ST"),
        std::span<const WorkloadExpectation>(kSimdStreamWriteExpectations),
        &workloads::SimdStreamWrite,
        "scalar-stream-write",
        "Both demos walk the same 64 MiB store stream. The main difference is whether the stores retire as scalar integer stores or explicit NEON vector stores.",
    },
    {
        "random-page-write",
        "Random Page Write",
        "One store per shuffled page across a large footprint.",
        "The store stream keeps landing on cold pages, which drives both store misses and data-side translation churn.",
        kRandomWriteConfig,
        kRandomWriteCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | L1_STORE_MISS | DTLB_MISS | L2_TLB_MISS,
        std::span<const WorkloadExpectation>(kRandomWriteExpectations),
        &workloads::RandomPageWrite,
    },
    {
        "uncontended-atomic-cas",
        "Uncontended Atomic CAS",
        "Repeated compare-exchange updates to a private atomic word.",
        "This is the low-failure atomic baseline: the measured thread performs atomic updates without any competing writers.",
        kUncontendedAtomicConfig,
        kUncontendedAtomicCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("ATOMIC_OR_EXCLUSIVE_SUCC") |
            PerfCounter::Named("ATOMIC_OR_EXCLUSIVE_FAIL"),
        std::span<const WorkloadExpectation>(kUncontendedAtomicExpectations),
        &workloads::UncontendedAtomicCas,
    },
    {
        "contended-atomic-cas",
        "Contended Atomic CAS",
        "Repeated compare-exchange updates while helper threads race on the same atomic word.",
        "This is the high-failure atomic showcase: the measured thread runs the same compare-exchange loop, but competing writers keep forcing retries.",
        kContendedAtomicConfig,
        kContendedAtomicCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("ATOMIC_OR_EXCLUSIVE_SUCC") |
            PerfCounter::Named("ATOMIC_OR_EXCLUSIVE_FAIL"),
        std::span<const WorkloadExpectation>(kContendedAtomicExpectations),
        &workloads::ContendedAtomicCas,
        "uncontended-atomic-cas",
        "Both demos use the same compare-exchange loop. The main difference is whether other threads are simultaneously racing on the same atomic word.",
    },
    {
        "barrier-loop",
        "Barrier Loop",
        "A tight arithmetic loop with an explicit full memory barrier in every iteration.",
        "Use this to make barrier retirement visible directly: the arithmetic body stays simple, but each iteration executes a real ordering instruction.",
        kBarrierLoopConfig,
        kBarrierLoopCode,
        Group::StoreOrdering,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_BARRIER") |
            PerfCounter::Named("INST_ALL"),
        std::span<const WorkloadExpectation>(kBarrierExpectations),
        &workloads::BarrierLoop,
        "dense-integer-alu",
        "Both demos are tight register-heavy loops. The main difference is that this version executes a full memory barrier in every iteration.",
    },
    {
        "interrupt-storm",
        "Interrupt Storm",
        "A dense arithmetic loop while a helper thread floods the measured thread with signals.",
        "This is the dedicated asynchronous-preemption showcase: the core work is still simple arithmetic, but a helper thread keeps delivering SIGUSR1, so each kernel round trip steals cycles from the measured thread.",
        kInterruptStormConfig,
        kInterruptStormCode,
        Group::CoreExecution,
        Tier::Experimental,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_ALL"),
        std::span<const WorkloadExpectation>(kInterruptStormExpectations),
        &workloads::InterruptStorm,
        "dense-integer-alu",
        "Both demos run an arithmetic loop. The difference is that this version is repeatedly preempted by signal delivery while the dense ALU baseline runs uninterrupted, so the cycle (and retired-kernel-instruction) gap is the signal-handling overhead.",
    },
    {
        "store-order-friendly",
        "Store Order Friendly",
        "A store-then-load loop whose younger load never targets the store's slot.",
        "This is the low-conflict baseline for the store-order experiments: each iteration's load is routed to a slot disjoint from the store, so the load/store unit never has to squash a speculatively-issued load.",
        kStoreOrderFriendlyConfig,
        kStoreOrderFriendlyCode,
        Group::StoreOrdering,
        Tier::Experimental,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("ST_MEMORY_ORDER_VIOLATION_NONSPEC") |
            PerfCounter::Named("INST_LDST"),
        std::span<const WorkloadExpectation>(kStoreOrderFriendlyExpectations),
        &workloads::StoreOrderFriendly,
    },
    {
        "store-order-alias",
        "Store Order Alias",
        "A store-then-load loop whose younger load unpredictably aliases the store.",
        "This is the high-conflict counterpart to the friendly case: about half the loads target the same slot as the preceding store, chosen from the same random bits, so the memory-dependence predictor cannot delay them and the core must squash and replay the speculatively-issued load.",
        kStoreOrderAliasConfig,
        kStoreOrderAliasCode,
        Group::StoreOrdering,
        Tier::Experimental,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("ST_MEMORY_ORDER_VIOLATION_NONSPEC") |
            PerfCounter::Named("INST_LDST"),
        std::span<const WorkloadExpectation>(kStoreOrderAliasExpectations),
        &workloads::StoreOrderAlias,
        "store-order-friendly",
        "Both demos issue the same store-then-load pattern at the same rate over an identical instruction stream. The only difference is whether the younger load can land on the store's slot: never (friendly) or unpredictably about half the time (alias). The aliasing forces squash-and-replay in the load/store unit, which shows up as ~1.6x more cycles -- the dedicated ST_MEMORY_ORDER_VIOLATION event stays at zero on this core, so cycles are the signal to watch.",
    },
    {
        "page-stride-read",
        "Page-Stride Read",
        "One demand load per shuffled page.",
        "The access pattern is intentionally translation-hostile: it keeps the data footprint sparse enough to churn both first- and second-level TLBs.",
        kPageStrideConfig,
        kPageStrideCode,
        Group::TlbPageWalk,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | DTLB_MISS | L2_TLB_MISS |
            PerfCounter::Named("L1D_TLB_MISS_NONSPEC"),
        std::span<const WorkloadExpectation>(kPageStrideExpectations),
        &workloads::PageStrideRead,
    },
    {
        "first-touch-fault",
        "First-Touch Fault",
        "Create a fresh sparse file mapping and touch each page exactly once.",
        "This is the dedicated virtual-memory-fault showcase: the data is not just translation-hostile, it is file-backed and absent until the first write instantiates each page.",
        kFirstTouchFaultConfig,
        kFirstTouchFaultCode,
        Group::TlbPageWalk,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | DTLB_MISS | PerfCounter::Named("MMU_TABLE_WALK_DATA"),
        std::span<const WorkloadExpectation>(kFirstTouchFaultExpectations),
        &workloads::FirstTouchFault,
        "hot-seq-read",
        "Both demos ultimately touch memory, but this one creates a brand-new sparse file mapping and faults every page in on first write while the hot read baseline stays on already-resident data.",
    },
    {
        "aligned-x64-load",
        "Aligned 64B-Region Load",
        "Load from the start of each 64-byte region.",
        "This is the low split-load baseline: the access pattern is wide, but every load stays aligned inside its 64-byte region.",
        kAlignedX64Config,
        kAlignedX64Code,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kAlignedX64Expectations),
        &workloads::AlignedX64Load,
    },
    {
        "cross-x64-load",
        "Cross 64B-Region Load",
        "Load 64 bits starting at byte 60 of each 64-byte region.",
        "This isolates 64-byte split accesses. The data footprint is otherwise linear, so the main difference is the forced split at each region boundary.",
        kCrossX64Config,
        kCrossX64Code,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kCrossX64Expectations),
        &workloads::CrossX64Load,
        "aligned-x64-load",
        "Both demos issue the same number of 64-bit loads over the same backing store. The main difference is that this version forces each load to straddle a 64-byte boundary.",
    },
    {
        "aligned-page-load",
        "Aligned Page Load",
        "Load from the start of each page.",
        "This is the low cross-page baseline: each access touches one page and one 64-byte region only.",
        kAlignedPageConfig,
        kAlignedPageCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kAlignedPageExpectations),
        &workloads::AlignedPageLoad,
    },
    {
        "cross-page-load",
        "Cross Page Load",
        "Load 64 bits starting four bytes before each page boundary.",
        "This forces every access to straddle a page boundary. On this machine that offset also crosses a 64-byte region boundary, so both split counters should rise.",
        kCrossPageConfig,
        kCrossPageCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kCrossPageExpectations),
        &workloads::CrossPageLoad,
        "aligned-page-load",
        "Both demos touch one 64-bit word per page. The difference is that this version starts four bytes before the boundary, so each load has to span two pages.",
    },
    {
        "aligned-x64-store",
        "Aligned 64B-Region Store",
        "Store 64 bits at the start of each 64-byte region.",
        "This is the low split-store baseline: the store stream is wide, but every store stays aligned inside its 64-byte region.",
        kAlignedX64StoreConfig,
        kAlignedX64StoreCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kAlignedX64StoreExpectations),
        &workloads::AlignedX64Store,
    },
    {
        "cross-x64-store",
        "Cross 64B-Region Store",
        "Store 64 bits starting at byte 60 of each 64-byte region.",
        "This isolates 64-byte split stores. The footprint and stride are otherwise the same as the aligned store baseline.",
        kCrossX64StoreConfig,
        kCrossX64StoreCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kCrossX64StoreExpectations),
        &workloads::CrossX64Store,
        "aligned-x64-store",
        "Both demos issue the same number of 64-bit stores over the same backing store. The main difference is that this version forces each store to straddle a 64-byte boundary.",
    },
    {
        "aligned-page-store",
        "Aligned Page Store",
        "Store 64 bits at the start of each page.",
        "This is the low cross-page store baseline: each store stays completely inside one page and one 64-byte region.",
        kAlignedPageStoreConfig,
        kAlignedPageStoreCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kAlignedPageStoreExpectations),
        &workloads::AlignedPageStore,
    },
    {
        "cross-page-store",
        "Cross Page Store",
        "Store 64 bits starting four bytes before each page boundary.",
        "This forces every store to span two pages. On this machine that offset also crosses a 64-byte region boundary, so both split counters should rise.",
        kCrossPageStoreConfig,
        kCrossPageStoreCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("LDST_X64_UOP") |
            PerfCounter::Named("LDST_XPG_UOP"),
        std::span<const WorkloadExpectation>(kCrossPageStoreExpectations),
        &workloads::CrossPageStore,
        "aligned-page-store",
        "Both demos touch one 64-bit word per page. The difference is that this version starts four bytes before the boundary, so each store has to span two pages.",
    },
    {
        "simd-vector-alu",
        "SIMD Vector ALU",
        "A dense compute loop built from NEON integer vector arithmetic.",
        "This is the SIMD-compute counterpart to dense integer ALU: the working set stays in registers, but the arithmetic retires on vector lanes instead of scalar integer ALUs.",
        kSimdAluConfig,
        kSimdAluCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("INST_SIMD_ALU"),
        std::span<const WorkloadExpectation>(kSimdAluExpectations),
        &workloads::SimdVectorAlu,
        "dense-integer-alu",
        "Both demos are dense compute loops with almost no memory pressure. The main difference is whether the arithmetic retires in SIMD lanes or scalar integer ALUs.",
    },
    {
        "dispatch-int-alu",
        "Dispatch Int ALU",
        "The dense scalar integer ALU loop measured through active-cycle and mapper/uop counters.",
        "Use this when you want to inspect how a clean scalar compute loop looks to the mapper and retire stages rather than focusing on high-level instructions alone.",
        kDispatchIntConfig,
        kDispatchIntCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("CORE_ACTIVE_CYCLE") |
            PerfCounter::Named("RETIRE_UOP") | PerfCounter::Named("MAP_UOP") |
            PerfCounter::Named("MAP_INT_UOP"),
        std::span<const WorkloadExpectation>(kDispatchIntExpectations),
        &workloads::DenseIntegerAlu,
        "dispatch-simd-alu",
        "Both demos are dense register-resident compute loops. The main difference is whether the mapper sees mostly scalar integer uops or mostly SIMD uops.",
    },
    {
        "dispatch-memory-stream",
        "Dispatch Memory Stream",
        "The scalar stream-read loop measured through active-cycle and mapper/uop counters.",
        "This keeps the code simple but shifts the mapped work toward load/store uops instead of scalar or SIMD arithmetic.",
        kDispatchMemoryConfig,
        kDispatchMemoryCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("CORE_ACTIVE_CYCLE") |
            PerfCounter::Named("RETIRE_UOP") | PerfCounter::Named("MAP_UOP") |
            PerfCounter::Named("MAP_LDST_UOP"),
        std::span<const WorkloadExpectation>(kDispatchMemoryExpectations),
        &workloads::ScalarStreamRead,
        "dispatch-int-alu",
        "Both demos are steady-state loops with little control-flow noise. The difference is whether the mapped work is mostly scalar integer arithmetic or mostly load/store traffic.",
    },
    {
        "dispatch-simd-alu",
        "Dispatch SIMD ALU",
        "The dense NEON vector ALU loop measured through active-cycle and mapper/uop counters.",
        "This is the SIMD-side mapper view: it keeps the loop register-resident while shifting the mapped work into vector uops.",
        kDispatchSimdConfig,
        kDispatchSimdCode,
        Group::SimdUopMapping,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("CORE_ACTIVE_CYCLE") |
            PerfCounter::Named("RETIRE_UOP") | PerfCounter::Named("MAP_UOP") |
            PerfCounter::Named("MAP_SIMD_UOP"),
        std::span<const WorkloadExpectation>(kDispatchSimdExpectations),
        &workloads::SimdVectorAlu,
        "dispatch-int-alu",
        "Both demos are dense register-resident compute loops. The main difference is whether the mapper sees mostly SIMD uops or mostly scalar integer uops.",
    },
    {
        "predictable-branch",
        "Predictable Branch",
        "A branch-heavy loop with strongly biased direction.",
        "This drives retired branch count without paying much misprediction cost because the branch outcome is easy for the predictor.",
        kPredictableBranchConfig,
        kPredictableBranchCode,
        Group::BranchControl,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | BRANCHES | BRANCH_MISS |
            PerfCounter::Named("INST_BRANCH_COND") |
            PerfCounter::Named("BRANCH_COND_MISPRED_NONSPEC") |
            PerfCounter::Named("INST_BRANCH_TAKEN"),
        std::span<const WorkloadExpectation>(kPredictableBranchExpectations),
        &workloads::PredictableBranch,
    },
    {
        "unpredictable-branch",
        "Unpredictable Branch",
        "A branch-heavy loop driven by random direction bits.",
        "The conditional branch stays frequent, but now the predictor sees near-random direction and loses accuracy.",
        kUnpredictableBranchConfig,
        kUnpredictableBranchCode,
        Group::BranchControl,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | BRANCHES | BRANCH_MISS |
            PerfCounter::Named("INST_BRANCH_COND") |
            PerfCounter::Named("BRANCH_COND_MISPRED_NONSPEC") |
            PerfCounter::Named("INST_BRANCH_TAKEN"),
        std::span<const WorkloadExpectation>(kUnpredictableBranchExpectations),
        &workloads::UnpredictableBranch,
    },
    {
        "hot-instruction-loop",
        "Hot Instruction Loop",
        "Repeatedly call the same tiny executable stub.",
        "This is the frontend low-miss baseline: one small code page stays hot in L1I and the ITLB.",
        kHotInstructionConfig,
        kHotInstructionCode,
        Group::InstructionSide,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | ITLB_MISS | PerfCounter::Named("L1I_CACHE_MISS_DEMAND") |
            PerfCounter::Named("L1I_TLB_FILL") |
            PerfCounter::Named("L2_TLB_MISS_INSTRUCTION") |
            PerfCounter::Named("MMU_TABLE_WALK_INSTRUCTION"),
        std::span<const WorkloadExpectation>(kHotInstructionExpectations),
        &workloads::HotInstructionLoop,
    },
    {
        "random-instruction-pages",
        "Random Instruction Pages",
        "Jump across many executable pages in randomized order.",
        "The frontend has to keep rediscovering code pages, which raises instruction-cache and instruction-TLB pressure.",
        kRandomInstructionConfig,
        kRandomInstructionCode,
        Group::InstructionSide,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | ITLB_MISS | PerfCounter::Named("L1I_CACHE_MISS_DEMAND") |
            PerfCounter::Named("L1I_TLB_FILL") |
            PerfCounter::Named("L2_TLB_MISS_INSTRUCTION") |
            PerfCounter::Named("MMU_TABLE_WALK_INSTRUCTION"),
        std::span<const WorkloadExpectation>(kRandomInstructionExpectations),
        &workloads::RandomInstructionPages,
    },
    {
        "frontend-hot-restart",
        "Frontend Hot Restart",
        "The tiny hot executable stub loop measured through frontend restart counters.",
        "This is the low-restart frontend baseline: one small hot code page should let fetch run smoothly.",
        kFrontendHotRestartConfig,
        kFrontendHotRestartCode,
        Group::InstructionSide,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("FETCH_RESTART") |
            PerfCounter::Named("FLUSH_RESTART_OTHER_NONSPEC") |
            PerfCounter::Named("CORE_ACTIVE_CYCLE"),
        std::span<const WorkloadExpectation>(kFrontendHotRestartExpectations),
        &workloads::FrontendHotRestart,
    },
    {
        "frontend-random-restart",
        "Frontend Random Restart",
        "The randomized executable-page walk measured through frontend restart counters.",
        "This is the high-restart frontend showcase: the code footprint keeps moving, so fetch repeatedly has to restart from new places.",
        kFrontendRandomRestartConfig,
        kFrontendRandomRestartCode,
        Group::InstructionSide,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("FETCH_RESTART") |
            PerfCounter::Named("CORE_ACTIVE_CYCLE"),
        std::span<const WorkloadExpectation>(kFrontendRandomRestartExpectations),
        &workloads::FrontendRandomRestart,
        "frontend-hot-restart",
        "Both demos perform the same total number of indirect calls into generated executable stubs. The difference is whether every call targets one hot stub or the call stream walks a wide shuffled code-page set.",
    },
    {
        "frontend-self-modifying-restart",
        "Frontend Self-Modifying Restart",
        "Alternating executable stubs are rewritten and re-entered repeatedly.",
        "This is the dedicated non-branch frontend flush showcase: the code stream itself changes under execution, so the frontend has to absorb repeated invalidation and restart pressure rather than just code-page churn.",
        kFrontendSelfModifyingConfig,
        kFrontendSelfModifyingCode,
        Group::InstructionSide,
        Tier::Stable,
        3,
        1,
        CYCLES | INSTRUCTIONS | PerfCounter::Named("FETCH_RESTART") |
            PerfCounter::Named("FLUSH_RESTART_OTHER_NONSPEC") |
            PerfCounter::Named("CORE_ACTIVE_CYCLE"),
        std::span<const WorkloadExpectation>(kFrontendSelfModifyingExpectations),
        &workloads::FrontendSelfModifyingRestart,
        "frontend-hot-restart",
        "Both demos execute tiny generated stubs. The difference is that this version rewrites the active code, invalidates the instruction cache, and re-enters execution repeatedly while the hot baseline keeps the code stream fixed.",
    },
};

const CounterDefinition kCounters[] = {
    {
        "cycles",
        "Cycles",
        "Fixed core cycle counter. Useful for top-level latency and CPI comparisons.",
        "Interpret this alongside instructions or the highlighted miss counter. Raw cycles alone mostly tell you how stalled the workload became.",
        Group::CoreExecution,
        Tier::Stable,
        CYCLES,
        "random-pointer-chase",
        "dense-integer-alu",
        {3.0, 50'000},
    },
    {
        "instructions",
        "Instructions",
        "Fixed retired instruction counter.",
        "This is the cleanest way to show instruction density. It is not itself a stall signal.",
        Group::CoreExecution,
        Tier::Stable,
        INSTRUCTIONS,
        "dense-integer-alu",
        "random-pointer-chase",
        {3.0, 50'000},
    },
    {
        "branches",
        "Branches",
        "Retired branch instructions.",
        "Use this to compare branch-heavy control flow against straighter-line compute kernels.",
        Group::BranchControl,
        Tier::Stable,
        BRANCHES,
        "unpredictable-branch",
        "dense-integer-alu",
        {2.0, 10'000},
    },
    {
        "branch-miss",
        "Branch Mispredict",
        "Non-speculative branch mispredictions.",
        "This counter is most meaningful when paired with a branch-heavy workload; otherwise the denominator is tiny.",
        Group::BranchControl,
        Tier::Stable,
        BRANCH_MISS,
        "unpredictable-branch",
        "predictable-branch",
        {4.0, 1'000},
    },
    {
        "l1-load-miss",
        "L1D Load Miss",
        "L1 data-cache misses on loads.",
        "Large random dependent loads are the clearest way to light this up; hot read loops should stay near zero.",
        Group::MemoryCache,
        Tier::Stable,
        L1_LOAD_MISS,
        "random-pointer-chase",
        "hot-seq-read",
        {8.0, 1'000},
    },
    {
        "l1-store-miss",
        "L1D Store Miss",
        "L1 data-cache misses on stores.",
        "Cold page-scattered stores are the strongest stable trigger in this suite.",
        Group::StoreOrdering,
        Tier::Stable,
        L1_STORE_MISS,
        "random-page-write",
        "hot-seq-write",
        {8.0, 1'000},
    },
    {
        "dtlb-miss",
        "DTLB Miss",
        "First-level data-TLB misses.",
        "Sparse page-granular access patterns are the main showcase. Hot in-page access should stay low.",
        Group::TlbPageWalk,
        Tier::Stable,
        DTLB_MISS,
        "page-stride-read",
        "hot-seq-read",
        {8.0, 1'000},
    },
    {
        "itlb-miss",
        "ITLB Miss",
        "Demand instruction-TLB misses.",
        "This suite uses generated executable stubs spread across many pages so the counter has an explicit frontend trigger.",
        Group::InstructionSide,
        Tier::Stable,
        ITLB_MISS,
        "random-instruction-pages",
        "hot-instruction-loop",
        {8.0, 100},
    },
    {
        "l2-tlb-miss",
        "L2 Data TLB Miss",
        "Second-level data-side TLB misses.",
        "Despite the `l2` name, this is the data-side L2 TLB event, not a generic L2 cache miss.",
        Group::TlbPageWalk,
        Tier::Stable,
        L2_TLB_MISS,
        "page-stride-read",
        "hot-seq-read",
        {8.0, 100},
    },
    {
        "l1i-cache-miss",
        "L1I Demand Miss",
        "Instruction-cache misses on demand fetch.",
        "Experimental because frontend behavior can vary more with code shape and core type than the stable tier counters.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("L1I_CACHE_MISS_DEMAND"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "l1-load-miss-nonspec",
        "L1D Load Miss Nonspec",
        "Non-speculative variant of the L1D load-miss event.",
        "Experimental because it is closely related to the stable load-miss counter but has more implementation-specific semantics.",
        Group::MemoryCache,
        Tier::Stable,
        PerfCounter::Named("L1D_CACHE_MISS_LD_NONSPEC"),
        "random-pointer-chase",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "l1-store-miss-nonspec",
        "L1D Store Miss Nonspec",
        "Non-speculative variant of the L1D store-miss event.",
        "Experimental because write-allocate behavior can make the raw interpretation more subtle than the stable store-miss counter.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("L1D_CACHE_MISS_ST_NONSPEC"),
        "random-page-write",
        "hot-seq-write",
        {2.0, 100},
    },
    {
        "inst-all",
        "All Instructions",
        "Aggregate retired instruction count from the PMU event database.",
        "This should broadly track the fixed instruction counter, but it is kept experimental until it has been cross-checked more heavily.",
        Group::CoreExecution,
        Tier::Stable,
        PerfCounter::Named("INST_ALL"),
        "dense-integer-alu",
        "random-pointer-chase",
        {2.0, 100},
    },
    {
        "core-active-cycle",
        "Core Active Cycle",
        "Cycles where the core is actively doing useful work rather than sitting idle or fully stalled.",
        "Dense compute is the intended high case; pointer-chase latency is the intended low case.",
        Group::CoreExecution,
        Tier::Experimental,
        PerfCounter::Named("CORE_ACTIVE_CYCLE"),
        "dispatch-int-alu",
        "random-pointer-chase",
        {2.0, 100},
    },
    {
        "interrupt-pending",
        "Interrupt Pending",
        "Pending-interrupt pressure observed by the core.",
        "This is intentionally experimental because it depends on asynchronous signal delivery, but the interrupt-storm demo gives it a dedicated trigger instead of relying on background system noise.",
        Group::CoreExecution,
        Tier::Experimental,
        PerfCounter::Named("INTERRUPT_PENDING"),
        "interrupt-storm",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-int-alu",
        "Integer ALU Instructions",
        "Retired integer ALU instructions.",
        "Dense integer compute is the cleanest trigger; memory and branch workloads should stay much lower.",
        Group::CoreExecution,
        Tier::Stable,
        PerfCounter::Named("INST_INT_ALU"),
        "dense-integer-alu",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "retire-uop",
        "Retire Uops",
        "Micro-ops retired by the backend.",
        "This is a lower-level counterpart to instruction retirement; dense compute should be the clearest trigger.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("RETIRE_UOP"),
        "dispatch-int-alu",
        "random-pointer-chase",
        {2.0, 100},
    },
    {
        "map-uop",
        "Mapped Uops",
        "Micro-ops produced by the mapper.",
        "Use this as the broad frontend-to-backend uop flow counter before splitting into integer, SIMD, or load/store subclasses.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("MAP_UOP"),
        "dispatch-int-alu",
        "random-pointer-chase",
        {2.0, 100},
    },
    {
        "map-int-uop",
        "Mapped Integer Uops",
        "Integer-class uops produced by the mapper.",
        "The scalar integer ALU loop is the intended high case; the SIMD dispatch loop is the low contrast.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("MAP_INT_UOP"),
        "dispatch-int-alu",
        "dispatch-simd-alu",
        {2.0, 100},
    },
    {
        "inst-simd-ld",
        "SIMD Load Instructions",
        "Retired SIMD load instructions.",
        "The scalar and SIMD stream-read pair keeps the data pattern fixed while changing only the instruction class.",
        Group::MemoryCache,
        Tier::Stable,
        PerfCounter::Named("INST_SIMD_LD"),
        "simd-stream-read",
        "scalar-stream-read",
        {2.0, 100},
    },
    {
        "inst-int-ld",
        "Integer Load Instructions",
        "Retired integer load instructions.",
        "Use this to distinguish load-heavy kernels from register-only compute. The random and linear pointer chases are both good triggers.",
        Group::MemoryCache,
        Tier::Stable,
        PerfCounter::Named("INST_INT_LD"),
        "random-pointer-chase",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "ld-nt-uop",
        "Non-Temporal Load Uops",
        "Load-side uops that use the non-temporal path.",
        "The non-temporal stream-read demo keeps the footprint fixed and changes only the load path, which is the cleanest teaching case available here.",
        Group::MemoryCache,
        Tier::Stable,
        PerfCounter::Named("LD_NT_UOP"),
        "nt-stream-read",
        "scalar-stream-read",
        {2.0, 100},
    },
    {
        "inst-simd-st",
        "SIMD Store Instructions",
        "Retired SIMD store instructions.",
        "The scalar and SIMD stream-write pair keeps the store pattern fixed while changing only the instruction class.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("INST_SIMD_ST"),
        "simd-stream-write",
        "scalar-stream-write",
        {2.0, 100},
    },
    {
        "atomic-succ",
        "Atomic Success",
        "Successful atomic or exclusive operations.",
        "Uncontended compare-exchange is the clean success baseline; dense ALU code is the obvious low case.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("ATOMIC_OR_EXCLUSIVE_SUCC"),
        "uncontended-atomic-cas",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "atomic-fail",
        "Atomic Failure",
        "Failed atomic or exclusive operations.",
        "The contended compare-exchange demo is designed to force retries; the uncontended version should stay near zero.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("ATOMIC_OR_EXCLUSIVE_FAIL"),
        "contended-atomic-cas",
        "uncontended-atomic-cas",
        {2.0, 100},
    },
    {
        "inst-int-st",
        "Integer Store Instructions",
        "Retired integer store instructions.",
        "The random-page-write workload is the clearest store-side trigger in the current lab.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("INST_INT_ST"),
        "random-page-write",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "st-nt-uop",
        "Non-Temporal Store Uops",
        "Nominally store-side uops on the non-temporal path; on M4 P-cores it counts roughly all scalar store uops.",
        "Unlike ld-nt-uop (which is ldnp-specific), this fires for ordinary scalar stores too, so it does not isolate the non-temporal path and is effectively redundant with inst-int-st on this core; kept as a probe.",
        Group::StoreOrdering,
        Tier::Experimental,
        PerfCounter::Named("ST_NT_UOP"),
        "nt-stream-write",
        "hot-seq-write",
        {2.0, 100},
    },
    {
        "st-memory-order-violation",
        "Store Memory Order Violation",
        "Non-speculative store/load ordering violations.",
        "This remains experimental because the exact trigger is subtle and the dedicated event has stayed at zero on current M4 Max P-core runs, even with an explicit 4 KiB alias stress pair.",
        Group::StoreOrdering,
        Tier::Experimental,
        PerfCounter::Named("ST_MEMORY_ORDER_VIOLATION_NONSPEC"),
        "store-order-alias",
        "store-order-friendly",
        {2.0, 100},
    },
    {
        "inst-ldst",
        "Load/Store Instructions",
        "Retired load/store instructions of either kind.",
        "This is a broader memory-instruction mix counter, so the pointer-chase pair is a good teaching case.",
        Group::MemoryCache,
        Tier::Stable,
        PerfCounter::Named("INST_LDST"),
        "random-pointer-chase",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "map-ldst-uop",
        "Mapped Load/Store Uops",
        "Load/store-class uops produced by the mapper.",
        "The streaming memory demo is the intended high case; the scalar ALU loop is the low baseline.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("MAP_LDST_UOP"),
        "dispatch-memory-stream",
        "dispatch-int-alu",
        {2.0, 100},
    },
    {
        "ld-unit-uop",
        "Load-Unit Uops",
        "Micro-ops sent to the load unit.",
        "This tends to track load pressure more directly than retired load instructions.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("LD_UNIT_UOP"),
        "random-pointer-chase",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "st-unit-uop",
        "Store-Unit Uops",
        "Micro-ops sent to the store unit.",
        "The random page write is again the main high case; dense ALU code is the clean low case.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("ST_UNIT_UOP"),
        "random-page-write",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "l1d-writeback",
        "L1D Writeback",
        "L1 data-cache writebacks.",
        "Cold scattered stores are the clearest way to make dirty lines leave L1D in this suite.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("L1D_CACHE_WRITEBACK"),
        "random-page-write",
        "hot-seq-write",
        {2.0, 100},
    },
    {
        "dtlb-access",
        "DTLB Access",
        "Data-side first-level TLB accesses.",
        "Sparse page-stride access makes the translation machinery visible even before looking at misses.",
        Group::TlbPageWalk,
        Tier::Experimental,
        PerfCounter::Named("L1D_TLB_ACCESS"),
        "page-stride-read",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "dtlb-fill",
        "DTLB Fill",
        "Data-side first-level TLB fills.",
        "This rises when page working sets spill beyond the hot DTLB entries and translations have to be reinstalled.",
        Group::TlbPageWalk,
        Tier::Stable,
        PerfCounter::Named("L1D_TLB_FILL"),
        "page-stride-read",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "mmu-table-walk-data",
        "MMU Table Walk Data",
        "Data-side page table walks.",
        "A page-granular scattered footprint is the cleanest way to force real data-side walks.",
        Group::TlbPageWalk,
        Tier::Stable,
        PerfCounter::Named("MMU_TABLE_WALK_DATA"),
        "page-stride-read",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "mmu-virtual-memory-fault",
        "Virtual Memory Fault",
        "Non-speculative virtual-memory fault events on the data side.",
        "This remains experimental because the dedicated fault event has stayed at zero on current M4 Max P-core runs even when the workload clearly triggers translation churn and backing-page instantiation.",
        Group::TlbPageWalk,
        Tier::Experimental,
        PerfCounter::Named("MMU_VIRTUAL_MEMORY_FAULT_NONSPEC"),
        "first-touch-fault",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "inst-barrier",
        "Barrier Instructions",
        "Retired barrier instructions such as full memory fences.",
        "This is an ordering-focused teaching counter; the dedicated barrier loop should make it explicit.",
        Group::StoreOrdering,
        Tier::Stable,
        PerfCounter::Named("INST_BARRIER"),
        "barrier-loop",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-simd-alu",
        "SIMD ALU Instructions",
        "Retired SIMD arithmetic instructions.",
        "This is the vector-compute counterpart to the scalar integer ALU counter.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("INST_SIMD_ALU"),
        "simd-vector-alu",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "map-simd-uop",
        "Mapped SIMD Uops",
        "SIMD-class uops produced by the mapper.",
        "The vector ALU loop is the intended high case; the scalar dispatch loop is the low baseline.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("MAP_SIMD_UOP"),
        "dispatch-simd-alu",
        "dispatch-int-alu",
        {2.0, 100},
    },
    {
        "dtlb-miss-nonspec",
        "DTLB Miss Nonspec",
        "Non-speculative variant of the first-level data-side TLB miss counter.",
        "This should tell the same story as the stable DTLB miss counter, but with slightly different implementation semantics.",
        Group::TlbPageWalk,
        Tier::Stable,
        PerfCounter::Named("L1D_TLB_MISS_NONSPEC"),
        "page-stride-read",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "branch-cond-miss",
        "Conditional Branch Mispredict",
        "Conditional-branch-specific misprediction counter.",
        "Use this when you want the branch-miss story without including unrelated branch classes.",
        Group::BranchControl,
        Tier::Stable,
        PerfCounter::Named("BRANCH_COND_MISPRED_NONSPEC"),
        "unpredictable-branch",
        "predictable-branch",
        {4.0, 100},
    },
    {
        "map-rewind",
        "Map Rewind",
        "Mapper rewind events caused by speculative work being discarded.",
        "This is still an interpretive counter, but unpredictable branches are the cleanest rewind-heavy case currently in the lab.",
        Group::BranchControl,
        Tier::Stable,
        PerfCounter::Named("MAP_REWIND"),
        "unpredictable-branch",
        "predictable-branch",
        {2.0, 100},
    },
    {
        "inst-branch-cond",
        "Conditional Branch Instructions",
        "Retired conditional branch instructions.",
        "Branch-heavy loops are the obvious trigger; straight-line ALU code is the low case.",
        Group::BranchControl,
        Tier::Experimental,
        PerfCounter::Named("INST_BRANCH_COND"),
        "unpredictable-branch",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-branch-taken",
        "Taken Branch Instructions",
        "Retired taken branches.",
        "The biased branch workload is almost always taken, making it a good high case.",
        Group::BranchControl,
        Tier::Experimental,
        PerfCounter::Named("INST_BRANCH_TAKEN"),
        "predictable-branch",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-branch-call",
        "Call Instructions",
        "Retired branch-call instructions.",
        "The instruction-side workloads explicitly call a stub every iteration, so they are a natural teaching case.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("INST_BRANCH_CALL"),
        "hot-instruction-loop",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-branch-ret",
        "Return Instructions",
        "Retired branch-return instructions.",
        "Each stub invocation returns immediately, which makes the hot instruction loop the clean high case.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("INST_BRANCH_RET"),
        "hot-instruction-loop",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-branch-indir",
        "Indirect Branch Instructions",
        "Retired indirect branch instructions.",
        "The function-pointer stub call is an indirect branch, so the instruction-side workloads expose this clearly.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("INST_BRANCH_INDIR"),
        "hot-instruction-loop",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "branch-call-indir-miss",
        "Indirect Call Mispredict",
        "Indirect-call-specific misprediction counter.",
        "The randomized instruction-page sweep keeps changing the function-pointer target, which is the strongest indirect-call mispredict trigger in the current lab.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("BRANCH_CALL_INDIR_MISPRED_NONSPEC"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "branch-indir-miss",
        "Indirect Branch Mispredict",
        "General indirect-branch misprediction counter.",
        "The instruction-side function-pointer workloads already give us a clean changing-target versus fixed-target contrast for this event.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("BRANCH_INDIR_MISPRED_NONSPEC"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "branch-ret-indir-miss",
        "Indirect Return Mispredict",
        "Return-side indirect-branch misprediction counter.",
        "Experimental because return prediction is subtler than direct branch direction, but the randomized instruction-page sweep is still the strongest return-target stress case in this lab.",
        Group::InstructionSide,
        Tier::Experimental,
        PerfCounter::Named("BRANCH_RET_INDIR_MISPRED_NONSPEC"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "l1i-tlb-fill",
        "ITLB Fill",
        "Instruction-side first-level TLB fills.",
        "The random instruction-page sweep is the clearest high case because it keeps rediscovering code pages.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("L1I_TLB_FILL"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "l2-tlb-miss-instruction",
        "L2 Instruction TLB Miss",
        "Second-level instruction-side TLB misses.",
        "A wide randomized code footprint is the intended trigger; one hot code page is the low baseline.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("L2_TLB_MISS_INSTRUCTION"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "mmu-table-walk-instruction",
        "MMU Table Walk Instruction",
        "Instruction-side page table walks.",
        "This is the instruction-fetch analogue of the data-side walk counter.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("MMU_TABLE_WALK_INSTRUCTION"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "fetch-restart",
        "Fetch Restart",
        "Frontend fetch restart events.",
        "The random executable-page walk is the intended high case; a single hot stub is the low baseline.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("FETCH_RESTART"),
        "frontend-random-restart",
        "frontend-hot-restart",
        {2.0, 100},
    },
    {
        "flush-restart-other",
        "Flush Restart Other",
        "Non-branch frontend flush/restart events.",
        "Experimental because the exact microarchitectural meaning is opaque. On this machine, explicit code rewriting plus I-cache invalidation is a better probe than plain code-page churn.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("FLUSH_RESTART_OTHER_NONSPEC"),
        "frontend-self-modifying-restart",
        "frontend-hot-restart",
        {2.0, 100},
    },
    {
        "map-dispatch-bubble",
        "Map Dispatch Bubble",
        "Dispatch bubbles attributed to the mapper/front-end path.",
        "The randomized executable-page walk is the intended high case because it keeps fetch and decode from settling into a smooth steady state.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("MAP_DISPATCH_BUBBLE"),
        "frontend-random-restart",
        "frontend-hot-restart",
        {2.0, 100},
    },
    {
        "map-dispatch-bubble-ic",
        "Map Dispatch Bubble IC",
        "Dispatch bubbles attributed specifically to instruction-cache pressure.",
        "The instruction-page sweep is the clearest way to create repeated code-cache rediscovery in this project.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("MAP_DISPATCH_BUBBLE_IC"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "map-dispatch-bubble-itlb",
        "Map Dispatch Bubble ITLB",
        "Dispatch bubbles attributed specifically to instruction-TLB pressure.",
        "The random instruction-page walk intentionally destroys code-page locality, which is the cleanest ITLB-bubble showcase we currently have.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("MAP_DISPATCH_BUBBLE_ITLB"),
        "random-instruction-pages",
        "hot-instruction-loop",
        {2.0, 100},
    },
    {
        "map-stall",
        "Map Stall",
        "Mapper stall events.",
        "This is still an interpretive frontend counter, but the hot-stub versus randomized-code-page pair gives it an explicit locality-based contrast.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("MAP_STALL"),
        "frontend-random-restart",
        "frontend-hot-restart",
        {2.0, 100},
    },
    {
        "map-stall-dispatch",
        "Map Stall Dispatch",
        "Dispatch-facing mapper stall events.",
        "Use the same hot-versus-randomized frontend pair here; the goal is to expose whether fetch instability turns into dispatch-visible mapper stalls.",
        Group::InstructionSide,
        Tier::Stable,
        PerfCounter::Named("MAP_STALL_DISPATCH"),
        "frontend-random-restart",
        "frontend-hot-restart",
        {2.0, 100},
    },
    {
        "ldst-x64-uop",
        "64B Split Load/Store Uops",
        "Micro-ops created when an access straddles a 64-byte region boundary.",
        "This is best taught with aligned versus deliberately split 64-bit loads.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("LDST_X64_UOP"),
        "cross-x64-load",
        "aligned-x64-load",
        {8.0, 100},
    },
    {
        "ldst-xpg-uop",
        "Cross-Page Load/Store Uops",
        "Micro-ops created when an access spans a page boundary.",
        "Use the aligned-page versus cross-page load pair to make this obvious.",
        Group::SimdUopMapping,
        Tier::Stable,
        PerfCounter::Named("LDST_XPG_UOP"),
        "cross-page-load",
        "aligned-page-load",
        {8.0, 100},
    },
};

}  // namespace

std::span<const WorkloadDefinition> Workloads() {
  return std::span<const WorkloadDefinition>(kWorkloads);
}

std::span<const CounterDefinition> Counters() {
  return std::span<const CounterDefinition>(kCounters);
}

const WorkloadDefinition *FindWorkload(std::string_view id) {
  const auto workloads = Workloads();
  const auto it = std::find_if(workloads.begin(), workloads.end(),
                               [&](const WorkloadDefinition &workload) { return workload.id == id; });
  return it == workloads.end() ? nullptr : &(*it);
}

const CounterDefinition *FindCounter(std::string_view name) {
  const auto counters = Counters();
  const auto it = std::find_if(counters.begin(), counters.end(),
                               [&](const CounterDefinition &counter) { return counter.name == name; });
  return it == counters.end() ? nullptr : &(*it);
}

}  // namespace demo
