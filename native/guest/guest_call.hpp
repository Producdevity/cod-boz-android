#pragma once

#include "cpu/guest_cpu.hpp"
#include "guest/guest_memory.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace codboz {

class GuestArguments {
  public:
    explicit GuestArguments(GuestCpu &cpu) : cpu_(cpu) {}

    uint32_t u32();
    int32_t s32();
    GuestAddr pointer();
    uint64_t u64();
    float f32();
    double f64();

    static void setU64Return(GuestCpu &cpu, uint64_t value);
    static void setF32Return(GuestCpu &cpu, float value);
    static void setF64Return(GuestCpu &cpu, double value);

  private:
    uint32_t word(uint32_t index) const;
    void align64();

    GuestCpu &cpu_;
    uint32_t nextWord_ = 0;
};

struct GuestArgument {
    enum class Kind { U32, U64 };

    Kind kind = Kind::U32;
    uint64_t bits = 0;

    static GuestArgument u32(uint32_t value);
    static GuestArgument u64(uint64_t value);
    static GuestArgument f32(float value);
    static GuestArgument f64(double value);
};

enum class GuestCallStatus {
    Returned,
    Fault,
    Interrupted,
    SliceLimit,
    RecursionLimit,
    InvalidStack,
};

struct GuestCallResult {
    GuestCallStatus status = GuestCallStatus::Fault;
    RunOutcome outcome = RunOutcome::Fault;
    uint64_t returnValue = 0;
    GuestAddr finalPc = 0;
    uint32_t slices = 0;
    std::string detail;
};

struct GuestCallOptions {
    uint32_t maxRunSlices = 4096;
    std::function<bool()> interrupted;
};

GuestCallResult callGuest(GuestCpu &cpu, GuestMemory &memory, GuestCallback callback,
                          std::span<const GuestArgument> arguments,
                          const GuestCallOptions &options = {});

} // namespace codboz
