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

const WorkloadExpectation kLinearPointerExpectations[] = {
    {"cycles", "low", "The address stream is linear enough for the hardware prefetchers to stay useful, so the same number of dependent loads costs fewer cycles."},
    {"instructions", "low", "The loop body is still tiny; this is a latency baseline, not an instruction-density demo."},
    {"l1-load-miss", "low", "A linear walk keeps spatial locality intact, so miss pressure drops sharply versus the random chase."},
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
        "aligned-x64-load",
        "Aligned 64B-Region Load",
        "Load from the start of each 64-byte region.",
        "This is the low split-load baseline: the access pattern is wide, but every load stays aligned inside its 64-byte region.",
        kAlignedX64Config,
        kAlignedX64Code,
        Group::SimdUopMapping,
        Tier::Experimental,
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
        Tier::Experimental,
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
        Tier::Experimental,
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
        Tier::Experimental,
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
    {
        "inst-int-ld",
        "Integer Load Instructions",
        "Retired integer load instructions.",
        "Use this to distinguish load-heavy kernels from register-only compute. The random and linear pointer chases are both good triggers.",
        Group::MemoryCache,
        Tier::Experimental,
        PerfCounter::Named("INST_INT_LD"),
        "random-pointer-chase",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-int-st",
        "Integer Store Instructions",
        "Retired integer store instructions.",
        "The random-page-write workload is the clearest store-side trigger in the current lab.",
        Group::StoreOrdering,
        Tier::Experimental,
        PerfCounter::Named("INST_INT_ST"),
        "random-page-write",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "inst-ldst",
        "Load/Store Instructions",
        "Retired load/store instructions of either kind.",
        "This is a broader memory-instruction mix counter, so the pointer-chase pair is a good teaching case.",
        Group::MemoryCache,
        Tier::Experimental,
        PerfCounter::Named("INST_LDST"),
        "random-pointer-chase",
        "dense-integer-alu",
        {2.0, 100},
    },
    {
        "ld-unit-uop",
        "Load-Unit Uops",
        "Micro-ops sent to the load unit.",
        "This tends to track load pressure more directly than retired load instructions.",
        Group::SimdUopMapping,
        Tier::Experimental,
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
        Tier::Experimental,
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
        Tier::Experimental,
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
        Tier::Experimental,
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
        Tier::Experimental,
        PerfCounter::Named("MMU_TABLE_WALK_DATA"),
        "page-stride-read",
        "hot-seq-read",
        {2.0, 100},
    },
    {
        "ldst-x64-uop",
        "64B Split Load/Store Uops",
        "Micro-ops created when an access straddles a 64-byte region boundary.",
        "This is best taught with aligned versus deliberately split 64-bit loads.",
        Group::SimdUopMapping,
        Tier::Experimental,
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
        Tier::Experimental,
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
