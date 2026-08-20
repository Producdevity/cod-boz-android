#include "bridge/dispatcher.hpp"
#include "loader/guest_loader.hpp"

#include "test_macros.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

class TestCpu final : public codboz::GuestCpu {
  public:
    void reset() override {
        regs.fill(0);
        stack.fill(0);
    }
    codboz::RunOutcome run() override {
        return codboz::RunOutcome::Halted;
    }
    void halt() override {}
    uint32_t reg(int index) const override {
        return regs[static_cast<std::size_t>(index)];
    }
    void setReg(int index, uint32_t value) override {
        regs[static_cast<std::size_t>(index)] = value;
    }
    uint32_t cpsr() const override {
        return 0;
    }
    void setCpsr(uint32_t) override {}
    codboz::GuestAddr pc() const override {
        return 0;
    }
    void setPc(codboz::GuestAddr) override {}
    void setThumb(bool) override {}
    std::array<uint32_t, 64> fpRegs() const override {
        return {};
    }
    void setFpRegs(const std::array<uint32_t, 64> &) override {}
    uint32_t fpscr() const override {
        return 0;
    }
    void setFpscr(uint32_t) override {}
    uint32_t stackArg(int index) const override {
        return stack[static_cast<std::size_t>(index)];
    }

    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 8> stack{};
};

class FakeZeroConf final : public codboz::ZeroConfBackend {
  public:
    bool startSearch(uint32_t handle, const std::string &serviceType,
                     const std::string &domain) override {
        searchHandle = handle;
        type = serviceType;
        this->domain = domain;
        return allowSearch;
    }
    void stopSearch(uint32_t handle) override {
        stoppedHandle = handle;
    }
    bool publish(uint32_t handle, uint16_t port, const std::string &name,
                 const std::string &serviceType, const std::string &domain,
                 const std::vector<std::string> &txtRecords) override {
        publicationHandle = handle;
        this->port = port;
        this->name = name;
        type = serviceType;
        this->domain = domain;
        txt = txtRecords;
        return allowPublish;
    }
    bool update(uint32_t handle, const std::vector<std::string> &txtRecords) override {
        updatedHandle = handle;
        txt = txtRecords;
        return true;
    }
    void unpublish(uint32_t handle) override {
        unpublishedHandle = handle;
    }
    void shutdown() override {
        shutdownCalled = true;
    }

    bool allowSearch = true;
    bool allowPublish = true;
    bool shutdownCalled = false;
    uint32_t searchHandle = 0;
    uint32_t stoppedHandle = 0;
    uint32_t publicationHandle = 0;
    uint32_t updatedHandle = 0;
    uint32_t unpublishedHandle = 0;
    uint16_t port = 0;
    std::string name;
    std::string type;
    std::string domain;
    std::vector<std::string> txt;
};

} // namespace

int main() {
    using namespace codboz;
    constexpr GuestAddr kTable = 0x1000;
    constexpr GuestAddr kType = 0x1100;
    constexpr GuestAddr kDomain = 0x1140;
    constexpr GuestAddr kName = 0x1180;
    constexpr GuestAddr kTxtValue = 0x11c0;
    constexpr GuestAddr kTxtPointers = 0x1200;

    GuestMemory memory;
    memory.mapRegion(0x1000, 0x1000);
    memory.writeString(kType, "_PROJECT_KIWI._tcp");
    memory.writeString(kDomain, "local");
    memory.writeString(kName, "Kino Lobby");
    memory.writeString(kTxtValue, "version=1");
    memory.write32(kTxtPointers, kTxtValue);

    std::vector<std::string> symbols{"s3eExtGetHash"};
    auto backend = std::make_unique<FakeZeroConf>();
    auto *fake = backend.get();
    Runtime::Config config;
    TestCpu cpu;
    {
        Runtime runtime(memory, symbols, config, {}, std::move(backend));
        cpu.setReg(0, 0x9f590656u);
        cpu.setReg(1, kTable);
        cpu.setReg(2, 20);
        runtime.dispatch(importTrapAddress(0), cpu);
        CHECK(cpu.reg(0) == 0);
        const GuestAddr start = memory.read32(kTable);
        const GuestAddr stop = memory.read32(kTable + 4);
        const GuestAddr publish = memory.read32(kTable + 8);
        const GuestAddr update = memory.read32(kTable + 12);
        const GuestAddr unpublish = memory.read32(kTable + 16);

        cpu.reset();
        cpu.setReg(0, kType);
        cpu.setReg(1, kDomain);
        cpu.setReg(2, 0x2001);
        cpu.setReg(3, 0x2003);
        cpu.stack[0] = 0x2005;
        cpu.stack[1] = 0xabc0;
        runtime.dispatch(start, cpu);
        const GuestHandle search = cpu.reg(0);
        CHECK(search != 0);
        CHECK(fake->searchHandle == search);
        CHECK(fake->type == "_PROJECT_KIWI._tcp");
        CHECK(fake->domain == "local");

        cpu.reset();
        cpu.setReg(0, SocketSystem::hostToNetwork16(10));
        cpu.setReg(1, kName);
        cpu.setReg(2, kType);
        cpu.setReg(3, kDomain);
        cpu.stack[0] = 1;
        cpu.stack[1] = kTxtPointers;
        runtime.dispatch(publish, cpu);
        const GuestHandle publication = cpu.reg(0);
        CHECK(publication != 0);
        CHECK(fake->publicationHandle == publication);
        CHECK(fake->port == 10);
        CHECK(fake->name == "Kino Lobby");
        CHECK(fake->txt.size() == 1);
        CHECK(fake->txt[0] == "version=1");

        cpu.reset();
        cpu.setReg(0, publication);
        cpu.setReg(1, 1);
        cpu.setReg(2, kTxtPointers);
        runtime.dispatch(update, cpu);
        CHECK(cpu.reg(0) == 0);
        CHECK(fake->updatedHandle == publication);

        cpu.reset();
        cpu.setReg(0, search);
        runtime.dispatch(stop, cpu);
        CHECK(cpu.reg(0) == 0);
        CHECK(fake->stoppedHandle == search);

        cpu.reset();
        cpu.setReg(0, publication);
        runtime.dispatch(unpublish, cpu);
        CHECK(cpu.reg(0) == 0);
        CHECK(fake->unpublishedHandle == publication);
    }
    return 0;
}
