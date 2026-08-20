#include "guest_call.hpp"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

namespace codboz {
namespace {

constexpr uint32_t kRegisterArgumentWords = 4;
constexpr uint32_t kMaximumCallbackDepth = 8;
thread_local uint32_t callbackDepth = 0;

class CpuStateGuard {
  public:
    explicit CpuStateGuard(GuestCpu &cpu)
        : cpu_(cpu), fpRegisters_(cpu.fpRegs()), cpsr_(cpu.cpsr()), fpscr_(cpu.fpscr()) {
        for (int index = 0; index < 16; ++index) {
            registers_[static_cast<std::size_t>(index)] = cpu.reg(index);
        }
    }

    ~CpuStateGuard() {
        for (int index = 0; index < 16; ++index) {
            cpu_.setReg(index, registers_[static_cast<std::size_t>(index)]);
        }
        cpu_.setCpsr(cpsr_);
        cpu_.setFpRegs(fpRegisters_);
        cpu_.setFpscr(fpscr_);
    }

    uint32_t stackPointer() const {
        return registers_[13];
    }

  private:
    GuestCpu &cpu_;
    std::array<uint32_t, 16> registers_{};
    std::array<uint32_t, 64> fpRegisters_{};
    uint32_t cpsr_ = 0;
    uint32_t fpscr_ = 0;
};

class CallbackDepthGuard {
  public:
    CallbackDepthGuard() {
        ++callbackDepth;
    }
    ~CallbackDepthGuard() {
        --callbackDepth;
    }
};

uint32_t aligned64(uint32_t word) {
    return (word + 1u) & ~1u;
}

} // namespace

uint32_t GuestArguments::word(uint32_t index) const {
    return index < kRegisterArgumentWords ? cpu_.reg(static_cast<int>(index))
                                          : cpu_.stackArg(static_cast<int>(index - 4u));
}

void GuestArguments::align64() {
    nextWord_ = aligned64(nextWord_);
}

uint32_t GuestArguments::u32() {
    return word(nextWord_++);
}

int32_t GuestArguments::s32() {
    return static_cast<int32_t>(u32());
}

GuestAddr GuestArguments::pointer() {
    return u32();
}

uint64_t GuestArguments::u64() {
    align64();
    const uint64_t low = word(nextWord_++);
    const uint64_t high = word(nextWord_++);
    return low | (high << 32u);
}

float GuestArguments::f32() {
    return std::bit_cast<float>(u32());
}

double GuestArguments::f64() {
    return std::bit_cast<double>(u64());
}

void GuestArguments::setU64Return(GuestCpu &cpu, uint64_t value) {
    cpu.setReg(0, static_cast<uint32_t>(value));
    cpu.setReg(1, static_cast<uint32_t>(value >> 32u));
}

void GuestArguments::setF32Return(GuestCpu &cpu, float value) {
    cpu.setReg(0, std::bit_cast<uint32_t>(value));
}

void GuestArguments::setF64Return(GuestCpu &cpu, double value) {
    setU64Return(cpu, std::bit_cast<uint64_t>(value));
}

GuestArgument GuestArgument::u32(uint32_t value) {
    return {Kind::U32, value};
}

GuestArgument GuestArgument::u64(uint64_t value) {
    return {Kind::U64, value};
}

GuestArgument GuestArgument::f32(float value) {
    return {Kind::U32, std::bit_cast<uint32_t>(value)};
}

GuestArgument GuestArgument::f64(double value) {
    return {Kind::U64, std::bit_cast<uint64_t>(value)};
}

GuestCallResult callGuest(GuestCpu &cpu, GuestMemory &memory, GuestCallback callback,
                          std::span<const GuestArgument> arguments,
                          const GuestCallOptions &options) {
    GuestCallResult result;
    if (!callback) {
        result.status = GuestCallStatus::Fault;
        result.detail = "null callback";
        return result;
    }
    if (callbackDepth >= kMaximumCallbackDepth) {
        result.status = GuestCallStatus::RecursionLimit;
        result.detail = "callback recursion limit";
        return result;
    }

    CpuStateGuard state(cpu);
    CallbackDepthGuard depth;
    std::array<uint32_t, kRegisterArgumentWords> registerWords{};
    std::array<bool, kRegisterArgumentWords> registerUsed{};
    std::vector<uint32_t> stackWords;
    uint32_t nextWord = 0;

    auto putWord = [&](uint32_t index, uint32_t value) {
        if (index < kRegisterArgumentWords) {
            registerWords[index] = value;
            registerUsed[index] = true;
            return;
        }
        const auto stackIndex = static_cast<std::size_t>(index - kRegisterArgumentWords);
        if (stackWords.size() <= stackIndex) {
            stackWords.resize(stackIndex + 1u);
        }
        stackWords[stackIndex] = value;
    };

    for (const auto &argument : arguments) {
        if (argument.kind == GuestArgument::Kind::U64) {
            nextWord = aligned64(nextWord);
            putWord(nextWord++, static_cast<uint32_t>(argument.bits));
            putWord(nextWord++, static_cast<uint32_t>(argument.bits >> 32u));
        } else {
            putWord(nextWord++, static_cast<uint32_t>(argument.bits));
        }
    }

    const uint64_t stackBytes = static_cast<uint64_t>(stackWords.size()) * sizeof(uint32_t);
    if (stackBytes > std::numeric_limits<uint32_t>::max() || stackBytes > state.stackPointer()) {
        result.status = GuestCallStatus::InvalidStack;
        result.detail = "callback stack overflow";
        return result;
    }
    const GuestAddr callStack =
        static_cast<GuestAddr>((state.stackPointer() - static_cast<uint32_t>(stackBytes)) & ~7u);
    if (!stackWords.empty() && !memory.isMapped(callStack, static_cast<GuestSize>(stackBytes))) {
        result.status = GuestCallStatus::InvalidStack;
        result.detail = "callback stack is not mapped";
        return result;
    }

    try {
        for (std::size_t index = 0; index < stackWords.size(); ++index) {
            memory.write32(callStack + static_cast<GuestAddr>(index * sizeof(uint32_t)),
                           stackWords[index]);
        }
        for (int index = 0; index < static_cast<int>(kRegisterArgumentWords); ++index) {
            if (registerUsed[static_cast<std::size_t>(index)]) {
                cpu.setReg(index, registerWords[static_cast<std::size_t>(index)]);
            }
        }
        cpu.setReg(13, callStack);
        cpu.setReg(14, kCallbackReturnTrap);
        cpu.setThumb((callback & 1u) != 0);
        cpu.setPc(callback & ~1u);

        do {
            if (options.interrupted && options.interrupted()) {
                result.status = GuestCallStatus::Interrupted;
                result.outcome = RunOutcome::Halted;
                break;
            }
            result.outcome = cpu.run();
            ++result.slices;
            if (result.outcome == RunOutcome::CallbackReturned) {
                result.status = GuestCallStatus::Returned;
                break;
            }
            if (result.outcome == RunOutcome::Fault) {
                result.status = GuestCallStatus::Fault;
                break;
            }
        } while (result.slices < options.maxRunSlices);

        if (result.outcome == RunOutcome::Halted && result.slices >= options.maxRunSlices) {
            result.status = GuestCallStatus::SliceLimit;
            result.detail = "callback run-slice limit";
        }
        result.finalPc = cpu.pc();
        result.returnValue =
            static_cast<uint64_t>(cpu.reg(0)) | (static_cast<uint64_t>(cpu.reg(1)) << 32u);
    } catch (const std::exception &error) {
        result.status = GuestCallStatus::Fault;
        result.outcome = RunOutcome::Fault;
        result.detail = error.what();
    }
    return result;
}

} // namespace codboz
