#pragma once

#include "cpu/guest_cpu.hpp"
#include "guest/guest_memory.hpp"

#include <memory>
#include <span>

namespace codboz {

// Dynarmic A32 JIT guest CPU. Guest memory is the checked page-backed space;
// import traps are resolved through the supplied handler.
class DynarmicCpu : public GuestCpu {
  public:
    DynarmicCpu(GuestMemory &mem, ImportHandler handler,
                std::span<const GuestAddr> faultRecoveryPoints = {});
    ~DynarmicCpu() override;

    void reset() override;
    RunOutcome run() override;
    void halt() override;

    uint32_t reg(int i) const override;
    void setReg(int i, uint32_t value) override;
    uint32_t cpsr() const override;
    void setCpsr(uint32_t value) override;
    GuestAddr pc() const override;
    void setPc(GuestAddr addr) override;
    void setThumb(bool thumb) override;
    std::array<uint32_t, 64> fpRegs() const override;
    void setFpRegs(const std::array<uint32_t, 64> &values) override;
    uint32_t fpscr() const override;
    void setFpscr(uint32_t value) override;
    uint32_t stackArg(int index) const override;

    // Guest address of the access that caused the last Fault outcome.
    GuestAddr lastFaultAddr() const;
    // Populate the Dynarmic page table from guest memory for fast memory access.
    void syncPageTable();
    void clearCache();

    struct Impl; // named publicly so the in-TU callbacks can reference it

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace codboz
