#include "cpu/dynarmic_cpu.hpp"
#include "game/codboz_frame_interpolation.hpp"
#include "guest/guest_call.hpp"

#include "test_macros.hpp"

#include <array>
#include <cstdint>

int main() {
    using namespace codboz;
    constexpr GuestAddr imageBase = 0x4a000000u;
    constexpr GuestAddr stack = 0x70000000u;
    constexpr GuestAddr activeSet = 0x64000000u;
    constexpr GuestAddr activeSetPointer = imageBase + 0x45f2b0u;
    constexpr GuestAddr fixedSlot = imageBase + 0x3e6490u;
    constexpr GuestAddr interpolateSlot = imageBase + 0x3e6494u;
    constexpr GuestAddr fixedCallback = imageBase + 0x0ba98du;
    constexpr GuestAddr interpolateCallback = imageBase + 0x0ba7dbu;
    constexpr GuestAddr captureTransforms = imageBase + 0x0e8787u;
    constexpr GuestAddr applyFactor = imageBase + 0x0e87c9u;

    GuestMemory memory;
    memory.mapRegion(imageBase, 0x460000u);
    memory.mapRegion(stack, kGuestPageSize);
    memory.mapRegion(activeSet, kGuestPageSize);
    memory.write32(activeSetPointer, activeSet);
    memory.write32(fixedSlot, fixedCallback);
    memory.write32(interpolateSlot, interpolateCallback);

    const std::array<uint8_t, 6> captureStub = {0x01, 0x21, 0x01, 0x60, 0x70, 0x47};
    const std::array<uint8_t, 4> applyStub = {0x41, 0x60, 0x70, 0x47};
    const std::array<uint8_t, 4> fixedStub = {0x77, 0x20, 0x70, 0x47};
    const std::array<uint8_t, 2> interpolateStub = {0x70, 0x47};
    memory.write(captureTransforms & ~1u, captureStub.data(), captureStub.size());
    memory.write(applyFactor & ~1u, applyStub.data(), applyStub.size());
    memory.write(fixedCallback & ~1u, fixedStub.data(), fixedStub.size());
    memory.write(interpolateCallback & ~1u, interpolateStub.data(), interpolateStub.size());

    CHECK(CodbozFrameInterpolation::apply(memory, imageBase));
    CHECK(memory.read32(fixedSlot) == CodbozFrameInterpolation::fixedTrampoline());
    CHECK(memory.read32(interpolateSlot) == CodbozFrameInterpolation::interpolateTrampoline());
    CHECK(memory.read32(0x6300001cu) == activeSetPointer);
    CHECK(memory.read32(0x63000020u) == captureTransforms);
    CHECK(memory.read32(0x63000024u) == fixedCallback);
    CHECK(memory.read32(0x63000074u) == activeSetPointer);
    CHECK(memory.read32(0x63000078u) == applyFactor);

    DynarmicCpu cpu(memory, [](GuestAddr, GuestCpu &) {});
    cpu.syncPageTable();
    cpu.setReg(13, stack + kGuestPageSize - 0x10u);

    const auto invoke = [&](GuestAddr callback, uint32_t channel, GuestArgument factor) {
        const std::array arguments = {GuestArgument::u32(0), GuestArgument::u32(channel),
                                      GuestArgument::u32(0), factor};
        return callGuest(cpu, memory, callback, arguments);
    };

    auto result = invoke(memory.read32(fixedSlot), 1, GuestArgument::u32(0));
    CHECK(result.status == GuestCallStatus::Returned);
    CHECK(static_cast<uint32_t>(result.returnValue) == 0x77u);
    CHECK(memory.read32(activeSet) == 1u);

    memory.write32(activeSet, 0u);
    result = invoke(memory.read32(fixedSlot), 0, GuestArgument::u32(0));
    CHECK(result.status == GuestCallStatus::Returned);
    CHECK(static_cast<uint32_t>(result.returnValue) == 0x77u);
    CHECK(memory.read32(activeSet) == 0u);

    result = invoke(memory.read32(interpolateSlot), 1, GuestArgument::f32(0.25f));
    CHECK(result.status == GuestCallStatus::Returned);
    CHECK(memory.read32(activeSet + 4u) == 1024u);

    result = invoke(memory.read32(interpolateSlot), 1, GuestArgument::f32(2.0f));
    CHECK(result.status == GuestCallStatus::Returned);
    CHECK(memory.read32(activeSet + 4u) == 4096u);

    memory.write32(activeSet + 4u, 0u);
    result = invoke(memory.read32(interpolateSlot), 0, GuestArgument::f32(0.5f));
    CHECK(result.status == GuestCallStatus::Returned);
    CHECK(memory.read32(activeSet + 4u) == 0u);
    return 0;
}
