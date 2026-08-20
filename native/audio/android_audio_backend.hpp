#pragma once

#include "audio_backend.hpp"

#include <jni.h>

namespace codboz {

class AndroidAudioBackend final : public AudioBackend {
  public:
    AndroidAudioBackend(JNIEnv *env, jobject bridge);
    ~AndroidAudioBackend() override;

    AndroidAudioBackend(const AndroidAudioBackend &) = delete;
    AndroidAudioBackend &operator=(const AndroidAudioBackend &) = delete;

    bool available() const override;
    int32_t outputSampleRate() const override;
    bool playMusic(const std::string &path, uint32_t repeat) override;
    void stopMusic() override;
    void pauseMusic() override;
    void resumeMusic() override;
    int32_t musicStatus() const override;
    void setMusicVolume(int32_t volume) override;
    bool playSound(int32_t channel, std::span<const int16_t> samples, int32_t sampleRate,
                   int32_t volume) override;
    void stopSound(int32_t channel) override;
    void pauseSound(int32_t channel) override;
    void resumeSound(int32_t channel) override;
    void setSoundVolume(int32_t channel, int32_t volume) override;
    uint32_t activeSoundMask() const override;
    bool voiceAvailable() const override;
    bool startVoice() override;
    void stopVoice() override;
    std::optional<VoiceInputChunk> takeVoiceInput() override;
    bool queueVoiceOutput(std::span<const int16_t> samples, int32_t sampleRate) override;
    std::string voiceDiagnostics() const override;
    void shutdown() override;

  private:
    JNIEnv *environment(bool &attached) const;
    void detach(bool attached) const;
    bool clearException(JNIEnv *env) const;
    void callVoid(jmethodID method) const;

    JavaVM *vm_ = nullptr;
    jobject bridge_ = nullptr;
    jmethodID available_ = nullptr;
    jmethodID outputSampleRate_ = nullptr;
    jmethodID playMusic_ = nullptr;
    jmethodID stopMusic_ = nullptr;
    jmethodID pauseMusic_ = nullptr;
    jmethodID resumeMusic_ = nullptr;
    jmethodID musicStatus_ = nullptr;
    jmethodID setMusicVolume_ = nullptr;
    jmethodID playSound_ = nullptr;
    jmethodID stopSound_ = nullptr;
    jmethodID pauseSound_ = nullptr;
    jmethodID resumeSound_ = nullptr;
    jmethodID setSoundVolume_ = nullptr;
    jmethodID activeSoundMask_ = nullptr;
    jmethodID voiceAvailable_ = nullptr;
    jmethodID startVoice_ = nullptr;
    jmethodID stopVoice_ = nullptr;
    jmethodID takeVoiceInput_ = nullptr;
    jmethodID voiceInputSampleRate_ = nullptr;
    jmethodID queueVoiceOutput_ = nullptr;
    jmethodID voiceDiagnostics_ = nullptr;
    jmethodID shutdown_ = nullptr;
    bool shutdownCalled_ = false;
};

} // namespace codboz
