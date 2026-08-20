#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codboz {

struct VoiceInputChunk {
    std::vector<int16_t> samples;
    int32_t sampleRate = 0;
};

class AudioBackend {
  public:
    virtual ~AudioBackend() = default;

    virtual bool available() const = 0;
    virtual int32_t outputSampleRate() const = 0;
    virtual bool playMusic(const std::string &path, uint32_t repeat) = 0;
    virtual void stopMusic() = 0;
    virtual void pauseMusic() = 0;
    virtual void resumeMusic() = 0;
    virtual int32_t musicStatus() const = 0;
    virtual void setMusicVolume(int32_t volume) = 0;
    virtual bool playSound(int32_t channel, std::span<const int16_t> samples, int32_t sampleRate,
                           int32_t volume) = 0;
    virtual void stopSound(int32_t channel) = 0;
    virtual void pauseSound(int32_t channel) = 0;
    virtual void resumeSound(int32_t channel) = 0;
    virtual void setSoundVolume(int32_t channel, int32_t volume) = 0;
    virtual uint32_t activeSoundMask() const = 0;
    virtual bool voiceAvailable() const = 0;
    virtual bool startVoice() = 0;
    virtual void stopVoice() = 0;
    virtual std::optional<VoiceInputChunk> takeVoiceInput() = 0;
    virtual bool queueVoiceOutput(std::span<const int16_t> samples, int32_t sampleRate) = 0;
    virtual std::string voiceDiagnostics() const = 0;
    virtual void shutdown() = 0;
};

} // namespace codboz
