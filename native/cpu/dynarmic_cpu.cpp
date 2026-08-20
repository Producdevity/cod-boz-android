#include "dynarmic_cpu.hpp"

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

#include <algorithm>

namespace codboz {

namespace {
constexpr uint32_t kThumbBit = 1u << 5;
constexpr uint32_t kSupervisorCall = 0xEF000000u;

uint32_t trapImmediate(GuestAddr address) {
    return (address - kExtTrapBase) / sizeof(uint32_t);
}

GuestAddr trapAddress(uint32_t immediate) {
    return kExtTrapBase + immediate * sizeof(uint32_t);
}
} // namespace

struct DynarmicCpu::Impl {
    GuestMemory &mem;
    ImportHandler handler;
    Dynarmic::A32::UserConfig config{};
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::shared_ptr<void> callbacks; // owns the UserCallbacks for the JIT's lifetime
    DynarmicCpu *self = nullptr;
    bool fault = false;
    GuestAddr lastFaultAddr = 0;
    std::vector<std::uint8_t *> pageTable;
    std::vector<GuestAddr> faultRecoveryPoints;

    Impl(GuestMemory &m, ImportHandler h) : mem(m), handler(std::move(h)) {}
};

namespace {

class Callbacks : public Dynarmic::A32::UserCallbacks {
  public:
    explicit Callbacks(DynarmicCpu::Impl &owner) : owner_(owner) {}

    bool PreCodeReadHook(bool, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (owner_.faultRecoveryPoints.empty()) {
            return true;
        }

        const auto isRecoveryPoint = [this](GuestAddr address) {
            return std::binary_search(owner_.faultRecoveryPoints.begin(),
                                      owner_.faultRecoveryPoints.end(), address);
        };
        const auto blockStart = Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC();

        if (isRecoveryPoint(pc) && ir.block.CycleCount() != 0) {
            ir.SetTerm(Dynarmic::IR::Term::LinkBlock{ir.current_location});
            return false;
        }
        if (isRecoveryPoint(blockStart) && ir.block.CycleCount() != 0) {
            ir.SetTerm(
                Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::LinkBlock{ir.current_location}});
            return false;
        }
        return true;
    }

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override {
        if (isTrapAddress(vaddr)) {
            if ((vaddr & (sizeof(uint32_t) - 1u)) != 0) {
                return std::nullopt;
            }
            return kSupervisorCall | trapImmediate(vaddr);
        }
        try {
            return owner_.mem.read32(vaddr);
        } catch (...) {
            // Unmapped code fetch: return nullopt to raise NoExecuteFault.
            // Returning 0 would let the JIT execute a NOP and continue through
            // the null page, which masks the real fault.
            return std::nullopt;
        }
    }

    std::uint8_t MemoryRead8(std::uint32_t v) override {
        return readOrFault8(v);
    }
    std::uint16_t MemoryRead16(std::uint32_t v) override {
        return readOrFault16(v);
    }
    std::uint32_t MemoryRead32(std::uint32_t v) override {
        return readOrFault32(v);
    }
    std::uint64_t MemoryRead64(std::uint32_t v) override {
        return readOrFault64(v);
    }

    void MemoryWrite8(std::uint32_t v, std::uint8_t value) override {
        writeOrFault(v, value);
    }
    void MemoryWrite16(std::uint32_t v, std::uint16_t value) override {
        writeOrFault(v, value);
    }
    void MemoryWrite32(std::uint32_t v, std::uint32_t value) override {
        writeOrFault(v, value);
    }
    void MemoryWrite64(std::uint32_t v, std::uint64_t value) override {
        writeOrFault(v, value);
    }

    void InterpreterFallback(std::uint32_t /*pc*/, std::size_t /*num*/) override {
        owner_.fault = true;
        owner_.jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void CallSVC(std::uint32_t swi) override {
        const GuestAddr pc = owner_.jit->Regs()[15];
        const GuestAddr address = trapAddress(swi);
        if (pc != address + sizeof(uint32_t) || !isTrapAddress(address)) {
            owner_.fault = true;
            owner_.jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
            return;
        }
        if (address == kCallbackReturnTrap) {
            owner_.jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        if (owner_.handler) {
            owner_.handler(address, *owner_.self);
        }
        returnFromImport();
    }

    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (pc == kCallbackReturnTrap) {
            owner_.jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        if (isTrapAddress(pc) && (exception == Dynarmic::A32::Exception::NoExecuteFault ||
                                  exception == Dynarmic::A32::Exception::UndefinedInstruction)) {
            if (owner_.handler) {
                owner_.handler(pc, *owner_.self);
            }
            returnFromImport();
            return;
        }
        owner_.fault = true;
        owner_.jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void AddTicks(std::uint64_t ticks) override {
        (void)ticks;
    }
    std::uint64_t GetTicksRemaining() override {
        return 0;
    }

  private:
    void returnFromImport() {
        const uint32_t lr = owner_.jit->Regs()[14];
        const uint32_t cpsr = owner_.jit->Cpsr();
        owner_.jit->SetCpsr((cpsr & ~kThumbBit) | ((lr & 1) ? kThumbBit : 0));
        owner_.jit->Regs()[15] = lr & ~1u;
    }

    std::uint8_t readOrFault8(std::uint32_t v) {
        try {
            return owner_.mem.read8(v);
        } catch (...) {
            signalFault(v);
            return 0;
        }
    }
    std::uint16_t readOrFault16(std::uint32_t v) {
        try {
            return owner_.mem.read16(v);
        } catch (...) {
            signalFault(v);
            return 0;
        }
    }
    std::uint32_t readOrFault32(std::uint32_t v) {
        try {
            return owner_.mem.read32(v);
        } catch (...) {
            signalFault(v);
            return 0;
        }
    }
    std::uint64_t readOrFault64(std::uint32_t v) {
        try {
            return owner_.mem.read64(v);
        } catch (...) {
            signalFault(v);
            return 0;
        }
    }
    template <typename T> void writeOrFault(std::uint32_t v, T value) {
        try {
            if constexpr (std::is_same_v<T, uint8_t>)
                owner_.mem.write8(v, value);
            else if constexpr (std::is_same_v<T, uint16_t>)
                owner_.mem.write16(v, value);
            else if constexpr (std::is_same_v<T, uint32_t>)
                owner_.mem.write32(v, value);
            else
                owner_.mem.write64(v, value);
        } catch (...) {
            signalFault(v);
        }
    }
    void signalFault(std::uint32_t addr) {
        owner_.lastFaultAddr = addr;
        owner_.fault = true;
        owner_.jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    }

    DynarmicCpu::Impl &owner_;
};

} // namespace

DynarmicCpu::DynarmicCpu(GuestMemory &mem, ImportHandler handler,
                         std::span<const GuestAddr> faultRecoveryPoints)
    : impl_(std::make_unique<Impl>(mem, std::move(handler))) {
    impl_->faultRecoveryPoints.assign(faultRecoveryPoints.begin(), faultRecoveryPoints.end());
    std::ranges::sort(impl_->faultRecoveryPoints);
    impl_->self = this;
    impl_->callbacks = std::make_shared<Callbacks>(*impl_);
    impl_->config.callbacks = static_cast<Dynarmic::A32::UserCallbacks *>(impl_->callbacks.get());
    impl_->config.enable_cycle_counting = false;
    impl_->config.check_halt_on_memory_access = impl_->faultRecoveryPoints.empty();
    impl_->config.unsafe_optimizations = false;
    impl_->config.fastmem_pointer = mem.fastmemPointer();
    impl_->config.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64;
    impl_->config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    impl_->pageTable.assign(1u << 20u, nullptr);
    impl_->config.page_table =
        reinterpret_cast<std::array<std::uint8_t *, 1 << 20> *>(impl_->pageTable.data());
    impl_->jit = std::make_unique<Dynarmic::A32::Jit>(impl_->config);
    impl_->jit->Reset();
}

DynarmicCpu::~DynarmicCpu() = default;

void DynarmicCpu::reset() {
    impl_->jit->Reset();
}

RunOutcome DynarmicCpu::run() {
    impl_->fault = false;
    auto hr = impl_->jit->Run();
    if (impl_->fault) {
        return RunOutcome::Fault;
    }
    if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined1)) {
        return RunOutcome::CallbackReturned;
    }
    return RunOutcome::Halted;
}

void DynarmicCpu::halt() {
    impl_->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
}

uint32_t DynarmicCpu::reg(int i) const {
    return impl_->jit->Regs()[i];
}
void DynarmicCpu::setReg(int i, uint32_t value) {
    impl_->jit->Regs()[i] = value;
}
uint32_t DynarmicCpu::cpsr() const {
    return impl_->jit->Cpsr();
}
void DynarmicCpu::setCpsr(uint32_t value) {
    impl_->jit->SetCpsr(value);
}
GuestAddr DynarmicCpu::pc() const {
    return impl_->jit->Regs()[15];
}
void DynarmicCpu::setPc(GuestAddr addr) {
    impl_->jit->Regs()[15] = addr;
}

void DynarmicCpu::setThumb(bool thumb) {
    uint32_t c = impl_->jit->Cpsr();
    impl_->jit->SetCpsr((c & ~kThumbBit) | (thumb ? kThumbBit : 0));
}

std::array<uint32_t, 64> DynarmicCpu::fpRegs() const {
    return impl_->jit->ExtRegs();
}

void DynarmicCpu::setFpRegs(const std::array<uint32_t, 64> &values) {
    impl_->jit->ExtRegs() = values;
}

uint32_t DynarmicCpu::fpscr() const {
    return impl_->jit->Fpscr();
}

void DynarmicCpu::setFpscr(uint32_t value) {
    impl_->jit->SetFpscr(value);
}

uint32_t DynarmicCpu::stackArg(int index) const {
    return impl_->mem.read32(impl_->jit->Regs()[13] + static_cast<uint32_t>(index) * 4u);
}

GuestAddr DynarmicCpu::lastFaultAddr() const {
    return impl_->lastFaultAddr;
}

void DynarmicCpu::syncPageTable() {
    impl_->mem.populatePageTable(impl_->pageTable.data(), impl_->pageTable.size());
}

void DynarmicCpu::clearCache() {
    impl_->jit->ClearCache();
}

} // namespace codboz
