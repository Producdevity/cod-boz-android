#include "audio/audio_backend.hpp"
#include "bridge/dispatcher.hpp"
#include "loader/guest_loader.hpp"
#include "loader/source_profile.hpp"

#include "test_macros.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace {

class TestCpu final : public codboz::GuestCpu {
  public:
    void reset() override {
        regs.fill(0);
    }
    codboz::RunOutcome run() override {
        return codboz::RunOutcome::Halted;
    }
    void halt() override {
        ++haltCount;
    }
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
    uint32_t stackArg(int) const override {
        return 0;
    }

    std::array<uint32_t, 16> regs{};
    int haltCount = 0;
};

class TestAudioBackend final : public codboz::AudioBackend {
  public:
    explicit TestAudioBackend(int32_t rate) : rate_(rate) {}

    bool available() const override {
        return true;
    }
    int32_t outputSampleRate() const override {
        return rate_;
    }
    bool playMusic(const std::string &, uint32_t) override {
        return true;
    }
    void stopMusic() override {
        ++musicStopCount;
    }
    void pauseMusic() override {
        ++musicPauseCount;
    }
    void resumeMusic() override {
        ++musicResumeCount;
    }
    int32_t musicStatus() const override {
        return musicState;
    }
    void setMusicVolume(int32_t value) override {
        musicVolume = value;
    }
    bool playSound(int32_t channel, std::span<const int16_t> samples, int32_t sampleRate,
                   int32_t volume) override {
        ++playCount;
        lastChannel = channel;
        lastSamples.assign(samples.begin(), samples.end());
        lastSampleRate = sampleRate;
        lastVolume = volume;
        return true;
    }
    void stopSound(int32_t) override {}
    void pauseSound(int32_t) override {}
    void resumeSound(int32_t) override {}
    void setSoundVolume(int32_t, int32_t) override {}
    uint32_t activeSoundMask() const override {
        return 0;
    }
    bool voiceAvailable() const override {
        return true;
    }
    bool startVoice() override {
        ++voiceStartCount;
        voiceStarted = true;
        return true;
    }
    void stopVoice() override {
        ++voiceStopCount;
        voiceStarted = false;
    }
    std::optional<codboz::VoiceInputChunk> takeVoiceInput() override {
        return std::nullopt;
    }
    bool queueVoiceOutput(std::span<const int16_t> samples, int32_t sampleRate) override {
        lastVoiceOutput.assign(samples.begin(), samples.end());
        lastVoiceOutputRate = sampleRate;
        return voiceStarted;
    }
    std::string voiceDiagnostics() const override {
        return voiceStarted ? "running=1" : "running=0";
    }
    void shutdown() override {}

    int playCount = 0;
    int lastChannel = -1;
    std::vector<int16_t> lastSamples;
    int32_t lastSampleRate = 0;
    int32_t lastVolume = 0;
    int musicStopCount = 0;
    int musicPauseCount = 0;
    int musicResumeCount = 0;
    int32_t musicVolume = -1;
    int32_t musicState = 1;
    int voiceStartCount = 0;
    int voiceStopCount = 0;
    bool voiceStarted = false;
    std::vector<int16_t> lastVoiceOutput;
    int32_t lastVoiceOutputRate = 0;

  private:
    int32_t rate_;
};

} // namespace

int main() {
    constexpr std::size_t kSoundGetInt = 0;
    constexpr std::size_t kSoundSetInt = 1;
    constexpr std::size_t kSoundChannelRegister = 2;
    constexpr std::size_t kSoundChannelPlay = 3;
    constexpr std::size_t kSoundChannelSetInt = 4;
    constexpr std::size_t kSoundChannelGetInt = 5;
    constexpr std::size_t kExtGetHash = 6;
    constexpr std::size_t kAudioSetInt = 7;
    constexpr std::size_t kAudioGetInt = 8;
    constexpr std::size_t kAudioIsPlaying = 9;
    const std::vector<std::string> symbols = {"s3eSoundGetInt",          "s3eSoundSetInt",
                                              "s3eSoundChannelRegister", "s3eSoundChannelPlay",
                                              "s3eSoundChannelSetInt",   "s3eSoundChannelGetInt",
                                              "s3eExtGetHash",           "s3eAudioSetInt",
                                              "s3eAudioGetInt",          "s3eAudioIsPlaying"};
    codboz::GuestMemory memory;
    auto audio = std::make_unique<TestAudioBackend>(48000);
    auto *audioState = audio.get();
    const auto controlConfig =
        std::filesystem::temp_directory_path() / "codboz-runtime-audio-config.txt";
    {
        std::ofstream output(controlConfig);
        output << "voice_chat_enabled=1\n";
    }
    codboz::Runtime::Config runtimeConfig;
    runtimeConfig.controlConfigPath = controlConfig.string();
    codboz::Runtime runtime(memory, symbols, runtimeConfig, std::move(audio));
    TestCpu cpu;

    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kSoundGetInt), cpu);
    CHECK(cpu.reg(0) == 256);
    cpu.setReg(0, 1);
    runtime.dispatch(codboz::importTrapAddress(kSoundGetInt), cpu);
    CHECK(cpu.reg(0) == 48000);
    cpu.setReg(0, 2);
    runtime.dispatch(codboz::importTrapAddress(kSoundGetInt), cpu);
    CHECK(cpu.reg(0) == 11000);

    cpu.setReg(0, 0);
    cpu.setReg(1, 128);
    runtime.dispatch(codboz::importTrapAddress(kSoundSetInt), cpu);
    CHECK(cpu.reg(0) == 0);
    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kSoundGetInt), cpu);
    CHECK(cpu.reg(0) == 128);

    cpu.setReg(0, 2);
    cpu.setReg(1, 22050);
    runtime.dispatch(codboz::importTrapAddress(kSoundSetInt), cpu);
    CHECK(cpu.reg(0) == 0);
    cpu.setReg(0, 2);
    runtime.dispatch(codboz::importTrapAddress(kSoundGetInt), cpu);
    CHECK(cpu.reg(0) == 22050);

    constexpr codboz::GuestAddr kPcm = 0x20000000;
    constexpr std::array<int16_t, 4> kSamples = {10, -20, 30, -40};
    memory.mapRegion(kPcm, 0x1000);
    memory.write(kPcm, reinterpret_cast<const uint8_t *>(kSamples.data()), sizeof(kSamples));

    constexpr codboz::GuestAddr kAudioInterface = kPcm + 0x100;
    cpu.setReg(0, codboz::extensionHash("IsAudioUnit"));
    cpu.setReg(1, kAudioInterface);
    cpu.setReg(2, 13 * sizeof(uint32_t));
    runtime.dispatch(codboz::importTrapAddress(kExtGetHash), cpu);
    CHECK(cpu.reg(0) == 0);

    runtime.dispatch(memory.read32(kAudioInterface + 2 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 0);
    runtime.dispatch(memory.read32(kAudioInterface + 5 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(audioState->voiceStartCount == 0);
    CHECK(!audioState->voiceStarted);
    cpu.setReg(0, 0x4a001001);
    cpu.setReg(1, 0x4a001101);
    runtime.dispatch(memory.read32(kAudioInterface + 4 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(audioState->voiceStartCount == 1);
    CHECK(audioState->voiceStarted);
    runtime.dispatch(memory.read32(kAudioInterface + 6 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(audioState->voiceStopCount == 1);
    CHECK(!audioState->voiceStarted);

    cpu.setReg(0, 0);
    cpu.setReg(1, 123);
    runtime.dispatch(memory.read32(kAudioInterface + 11 * sizeof(uint32_t)), cpu);
    CHECK(audioState->musicVolume == 123);
    CHECK(cpu.reg(0) == 0);

    cpu.setReg(0, 0);
    runtime.dispatch(memory.read32(kAudioInterface + 12 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 123);
    cpu.setReg(0, 1);
    runtime.dispatch(memory.read32(kAudioInterface + 12 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 1);
    cpu.setReg(0, 0xdeadbeef);
    runtime.dispatch(memory.read32(kAudioInterface + 12 * sizeof(uint32_t)), cpu);
    CHECK(cpu.reg(0) == 0xffffffffu);

    cpu.setReg(0, 0);
    cpu.setReg(1, 231);
    runtime.dispatch(codboz::importTrapAddress(kAudioSetInt), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(audioState->musicVolume == 231);
    cpu.setReg(0, 0);
    runtime.dispatch(codboz::importTrapAddress(kAudioGetInt), cpu);
    CHECK(cpu.reg(0) == 231);
    runtime.dispatch(codboz::importTrapAddress(kAudioIsPlaying), cpu);
    CHECK(cpu.reg(0) == 1);

    runtime.dispatch(memory.read32(kAudioInterface + 9 * sizeof(uint32_t)), cpu);
    runtime.dispatch(memory.read32(kAudioInterface + 10 * sizeof(uint32_t)), cpu);
    runtime.dispatch(memory.read32(kAudioInterface + 8 * sizeof(uint32_t)), cpu);
    CHECK(audioState->musicResumeCount == 1);
    CHECK(audioState->musicPauseCount == 1);
    CHECK(audioState->musicStopCount == 1);

    cpu.setReg(0, 0);
    cpu.setReg(1, 0);
    cpu.setReg(2, 0x8000);
    runtime.dispatch(codboz::importTrapAddress(kSoundChannelSetInt), cpu);
    CHECK(cpu.reg(0) == 0);
    cpu.setReg(0, 0);
    cpu.setReg(1, 1);
    runtime.dispatch(codboz::importTrapAddress(kSoundChannelGetInt), cpu);
    CHECK(cpu.reg(0) == 11025);

    cpu.setReg(0, 0);
    cpu.setReg(1, 1);
    cpu.setReg(2, 22050);
    runtime.dispatch(codboz::importTrapAddress(kSoundChannelSetInt), cpu);
    CHECK(cpu.reg(0) == 0);

    cpu.setReg(0, 0);
    cpu.setReg(1, kPcm);
    cpu.setReg(2, kSamples.size());
    cpu.setReg(3, 1);
    runtime.dispatch(codboz::importTrapAddress(kSoundChannelPlay), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(cpu.haltCount == 0);
    CHECK(audioState->playCount == 1);
    CHECK(audioState->lastChannel == 0);
    CHECK(audioState->lastSamples == std::vector<int16_t>(kSamples.begin(), kSamples.end()));
    CHECK(audioState->lastSampleRate == 22050);
    CHECK(audioState->lastVolume == 128);

    cpu.setReg(0, 1);
    cpu.setReg(1, 1);
    cpu.setReg(2, 0x4a001001);
    cpu.setReg(3, 0x20001000);
    runtime.dispatch(codboz::importTrapAddress(kSoundChannelRegister), cpu);
    CHECK(cpu.reg(0) == 0);

    cpu.setReg(0, 1);
    cpu.setReg(1, kPcm);
    cpu.setReg(2, kSamples.size());
    cpu.setReg(3, 0);
    runtime.dispatch(codboz::importTrapAddress(kSoundChannelPlay), cpu);
    CHECK(cpu.reg(0) == 0);
    CHECK(cpu.haltCount == 1);
    CHECK(audioState->playCount == 1);
    std::filesystem::remove(controlConfig);
    return 0;
}
