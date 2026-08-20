#pragma once

#include "guest/guest_address.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace codboz {

enum class RunOutcome {
    Halted,           // execution stopped via a halt trap
    CallbackReturned, // a host→guest callback returned through the return trap
    Fault,            // guest exception or unmapped access
};

class GuestCpu {
  public:
    virtual ~GuestCpu() = default;
    virtual void reset() = 0;
    virtual RunOutcome run() = 0;
    virtual void halt() = 0;

    virtual uint32_t reg(int i) const = 0;
    virtual void setReg(int i, uint32_t value) = 0;
    virtual uint32_t cpsr() const = 0;
    virtual void setCpsr(uint32_t value) = 0;
    virtual GuestAddr pc() const = 0;
    virtual void setPc(GuestAddr addr) = 0;
    virtual void setThumb(bool thumb) = 0;
    virtual std::array<uint32_t, 64> fpRegs() const = 0;
    virtual void setFpRegs(const std::array<uint32_t, 64> &values) = 0;
    virtual uint32_t fpscr() const = 0;
    virtual void setFpscr(uint32_t value) = 0;
    // Read AAPCS32 stack argument n (0-based, at sp + n*4).
    virtual uint32_t stackArg(int index) const = 0;
};

// Invoked for each guest import trap. Reads AAPCS32 arguments from the CPU
// registers and guest stack, performs the host work, and writes the return
// value(s) back to r0 (and r1 for 64-bit returns).
using ImportHandler = std::function<void(GuestAddr trapAddr, GuestCpu &cpu)>;

} // namespace codboz
