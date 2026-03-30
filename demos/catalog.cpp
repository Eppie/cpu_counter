#include "demos/catalog.h"

#include <algorithm>

namespace demo {
namespace {

const WorkloadExpectation kDenseAluExpectations[] = {
    {"cycles", "low", "The loop stays in registers and avoids long-latency miss penalties, so total cycle count stays comparatively low."},
    {"instructions", "high", "A tight integer ALU loop retires many instructions with almost no memory stalls."},
    {"branches", "low", "The loop body is mostly straight-line arithmetic, so branch density stays low."},
};

const WorkloadExpectation kHotReadExpectations[] = {
    {"cycles", "low", "The working set fits in L1D, so the core avoids miss-driven stalls."},
    {"l1-load-miss", "low", "Dependent loads stay resident in L1D instead of falling through to lower levels."},
    {"dtlb-miss", "low", "The address footprint is tiny, so DTLB pressure stays near zero."},
    {"l2-tlb-miss", "low", "A tiny hot footprint should not walk into second-level data TLB misses."},
};

const WorkloadExpectation kRandomPointerExpectations[] = {
    {"cycles", "high", "Each dependent load waits on the previous random miss, stretching cycle count sharply."},
    {"instructions", "low", "The loop body is instruction-light; most time is latency, not retire bandwidth."},
    {"l1-load-miss", "high", "Random dependent loads defeat prefetching and force frequent L1D misses."},
};

const WorkloadExpectation kHotWriteExpectations[] = {
    {"l1-store-miss", "low", "Repeated stores revisit the same hot cache lines, so store misses stay low."},
};

const WorkloadExpectation kRandomWriteExpectations[] = {
    {"l1-store-miss", "high", "One store per widely scattered page pushes writes onto cold lines."},
    {"dtlb-miss", "high", "Random page order churns the DTLB while issuing those stores."},
};

const WorkloadExpectation kPageStrideExpectations[] = {
    {"dtlb-miss", "high", "One demand access per shuffled page maximizes translation pressure."},
    {"l2-tlb-miss", "high", "Enough distinct pages overflow the first-level data TLB and spill into the next level."},
};

const WorkloadExpectation kPredictableBranchExpectations[] = {
    {"branches", "high", "The loop executes an actual conditional branch every iteration, so retired branch count is high."},
    {"branch-miss", "low", "The condition is strongly biased, so the predictor stays accurate."},
};

const WorkloadExpectation kUnpredictableBranchExpectations[] = {
    {"branches", "high", "The loop still executes a real conditional branch every iteration."},
    {"branch-miss", "high", "Random branch direction prevents the predictor from settling on an accurate history."},
};

const WorkloadExpectation kHotInstructionExpectations[] = {
    {"cycles", "low", "Calling the same tiny stub repeatedly keeps the frontend hot."},
    {"itlb-miss", "low", "One hot code page should not create ITLB pressure."},
    {"l1i-cache-miss", "low", "The same stub stays resident in the instruction cache."},
};

const WorkloadExpectation kRandomInstructionExpectations[] = {
    {"itlb-miss", "high", "Jumping across many executable pages churns the ITLB."},
    {"l1i-cache-miss", "high", "Randomized code-page order also defeats L1I locality."},
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

constexpr std::string_view kRandomPointerConfig =
    "2,000,000 dependent loads through a 32 MiB randomized ring.";
constexpr std::string_view kRandomPointerCode = R"cpp(
volatile const std::uint32_t *ring = state.random_ring.data();
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

constexpr std::string_view kRandomWriteConfig =
    "256 passes; one store per shuffled page across a 64 MiB footprint.";
constexpr std::string_view kRandomWriteCode = R"cpp(
for (std::size_t pass = 0; pass < 256; ++pass) {
  for (std::size_t page : state.page_order) {
    base[page * stride] += static_cast<std::uint64_t>(pass + 1);
  }
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
        CYCLES | INSTRUCTIONS | BRANCHES,
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
        CYCLES | INSTRUCTIONS | L1_LOAD_MISS | DTLB_MISS | L2_TLB_MISS,
        std::span<const WorkloadExpectation>(kHotReadExpectations),
        &workloads::HotSequentialRead,
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
        CYCLES | INSTRUCTIONS | DTLB_MISS | L2_TLB_MISS,
        std::span<const WorkloadExpectation>(kPageStrideExpectations),
        &workloads::PageStrideRead,
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
        CYCLES | INSTRUCTIONS | BRANCHES | BRANCH_MISS,
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
        CYCLES | INSTRUCTIONS | BRANCHES | BRANCH_MISS,
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
        CYCLES | INSTRUCTIONS | ITLB_MISS | PerfCounter::Named("L1I_CACHE_MISS_DEMAND"),
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
        CYCLES | INSTRUCTIONS | ITLB_MISS | PerfCounter::Named("L1I_CACHE_MISS_DEMAND"),
        std::span<const WorkloadExpectation>(kRandomInstructionExpectations),
        &workloads::RandomInstructionPages,
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
        "In this project `L2_MISS` is intentionally not treated as a generic L2 cache miss; it remains the data-side L2 TLB event.",
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
        Tier::Experimental,
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
        Tier::Experimental,
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
        Tier::Experimental,
        PerfCounter::Named("L1D_CACHE_MISS_ST_NONSPEC"),
        "random-page-write",
        "hot-seq-write",
        {2.0, 100},
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
