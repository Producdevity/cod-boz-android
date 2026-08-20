#include "android_audio_backend.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace codboz {

AndroidAudioBackend::AndroidAudioBackend(JNIEnv *env, jobject bridge) {
    if (!env || !bridge || env->GetJavaVM(&vm_) != JNI_OK) {
        throw std::runtime_error("invalid Android audio bridge");
    }
    bridge_ = env->NewGlobalRef(bridge);
    if (!bridge_) {
        throw std::runtime_error("could not retain Android audio bridge");
    }
    const jclass type = env->GetObjectClass(bridge_);
    available_ = env->GetMethodID(type, "isAvailable", "()Z");
    outputSampleRate_ = env->GetMethodID(type, "outputSampleRate", "()I");
    playMusic_ = env->GetMethodID(type, "playMusic", "(Ljava/lang/String;I)Z");
    stopMusic_ = env->GetMethodID(type, "stopMusic", "()V");
    pauseMusic_ = env->GetMethodID(type, "pauseMusic", "()V");
    resumeMusic_ = env->GetMethodID(type, "resumeMusic", "()V");
    musicStatus_ = env->GetMethodID(type, "musicStatus", "()I");
    setMusicVolume_ = env->GetMethodID(type, "setMusicVolume", "(I)V");
    playSound_ = env->GetMethodID(type, "playSound", "(I[SII)Z");
    stopSound_ = env->GetMethodID(type, "stopSound", "(I)V");
    pauseSound_ = env->GetMethodID(type, "pauseSound", "(I)V");
    resumeSound_ = env->GetMethodID(type, "resumeSound", "(I)V");
    setSoundVolume_ = env->GetMethodID(type, "setSoundVolume", "(II)V");
    activeSoundMask_ = env->GetMethodID(type, "activeSoundMask", "()I");
    voiceAvailable_ = env->GetMethodID(type, "isVoiceAvailable", "()Z");
    startVoice_ = env->GetMethodID(type, "startVoice", "()Z");
    stopVoice_ = env->GetMethodID(type, "stopVoice", "()V");
    takeVoiceInput_ = env->GetMethodID(type, "takeVoiceInput", "()[S");
    voiceInputSampleRate_ = env->GetMethodID(type, "voiceInputSampleRate", "()I");
    queueVoiceOutput_ = env->GetMethodID(type, "queueVoiceOutput", "([SI)Z");
    voiceDiagnostics_ = env->GetMethodID(type, "voiceDiagnostics", "()Ljava/lang/String;");
    shutdown_ = env->GetMethodID(type, "shutdown", "()V");
    env->DeleteLocalRef(type);
    if (clearException(env) || !available_ || !outputSampleRate_ || !playMusic_ || !stopMusic_ ||
        !pauseMusic_ || !resumeMusic_ || !musicStatus_ || !setMusicVolume_ || !playSound_ ||
        !stopSound_ || !pauseSound_ || !resumeSound_ || !setSoundVolume_ || !activeSoundMask_ ||
        !voiceAvailable_ || !startVoice_ || !stopVoice_ || !takeVoiceInput_ ||
        !voiceInputSampleRate_ || !queueVoiceOutput_ || !voiceDiagnostics_ || !shutdown_) {
        env->DeleteGlobalRef(bridge_);
        bridge_ = nullptr;
        throw std::runtime_error("incomplete Android audio bridge");
    }
}

AndroidAudioBackend::~AndroidAudioBackend() {
    shutdown();
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->DeleteGlobalRef(bridge_);
    }
    bridge_ = nullptr;
    detach(attached);
}

JNIEnv *AndroidAudioBackend::environment(bool &attached) const {
    attached = false;
    if (!vm_) {
        return nullptr;
    }
    JNIEnv *env = nullptr;
    const jint result = vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (result == JNI_OK) {
        return env;
    }
    if (result != JNI_EDETACHED || vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    attached = true;
    return env;
}

void AndroidAudioBackend::detach(bool attached) const {
    if (attached && vm_) {
        vm_->DetachCurrentThread();
    }
}

bool AndroidAudioBackend::clearException(JNIEnv *env) const {
    if (!env || !env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

bool AndroidAudioBackend::available() const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    const bool result = env && bridge_ && env->CallBooleanMethod(bridge_, available_) == JNI_TRUE &&
                        !clearException(env);
    detach(attached);
    return result;
}

int32_t AndroidAudioBackend::outputSampleRate() const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    int32_t result = 0;
    if (env && bridge_) {
        result = env->CallIntMethod(bridge_, outputSampleRate_);
        if (clearException(env)) {
            result = 0;
        }
    }
    detach(attached);
    return result;
}

bool AndroidAudioBackend::playMusic(const std::string &path, uint32_t repeat) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    bool result = false;
    if (env && bridge_) {
        jstring value = env->NewStringUTF(path.c_str());
        if (value) {
            result = env->CallBooleanMethod(bridge_, playMusic_, value,
                                            static_cast<jint>(repeat)) == JNI_TRUE &&
                     !clearException(env);
            env->DeleteLocalRef(value);
        }
    }
    detach(attached);
    return result;
}

void AndroidAudioBackend::callVoid(jmethodID method) const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, method);
        clearException(env);
    }
    detach(attached);
}

void AndroidAudioBackend::stopMusic() {
    callVoid(stopMusic_);
}

void AndroidAudioBackend::pauseMusic() {
    callVoid(pauseMusic_);
}

void AndroidAudioBackend::resumeMusic() {
    callVoid(resumeMusic_);
}

int32_t AndroidAudioBackend::musicStatus() const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    int32_t result = 0;
    if (env && bridge_) {
        result = env->CallIntMethod(bridge_, musicStatus_);
        if (clearException(env)) {
            result = 0;
        }
    }
    detach(attached);
    return result;
}

void AndroidAudioBackend::setMusicVolume(int32_t volume) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, setMusicVolume_, std::clamp(volume, 0, 256));
        clearException(env);
    }
    detach(attached);
}

bool AndroidAudioBackend::playSound(int32_t channel, std::span<const int16_t> samples,
                                    int32_t sampleRate, int32_t volume) {
    if (samples.empty() ||
        samples.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
        return false;
    }
    bool attached = false;
    JNIEnv *env = environment(attached);
    bool result = false;
    if (env && bridge_) {
        const auto length = static_cast<jsize>(samples.size());
        jshortArray pcm = env->NewShortArray(length);
        if (pcm) {
            env->SetShortArrayRegion(pcm, 0, length,
                                     reinterpret_cast<const jshort *>(samples.data()));
            if (!clearException(env)) {
                result = env->CallBooleanMethod(bridge_, playSound_, channel, pcm, sampleRate,
                                                std::clamp(volume, 0, 256)) == JNI_TRUE &&
                         !clearException(env);
            }
            env->DeleteLocalRef(pcm);
        }
    }
    detach(attached);
    return result;
}

void AndroidAudioBackend::stopSound(int32_t channel) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, stopSound_, channel);
        clearException(env);
    }
    detach(attached);
}

void AndroidAudioBackend::pauseSound(int32_t channel) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, pauseSound_, channel);
        clearException(env);
    }
    detach(attached);
}

void AndroidAudioBackend::resumeSound(int32_t channel) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, resumeSound_, channel);
        clearException(env);
    }
    detach(attached);
}

void AndroidAudioBackend::setSoundVolume(int32_t channel, int32_t volume) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, setSoundVolume_, channel, std::clamp(volume, 0, 256));
        clearException(env);
    }
    detach(attached);
}

uint32_t AndroidAudioBackend::activeSoundMask() const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    uint32_t result = 0;
    if (env && bridge_) {
        result = static_cast<uint32_t>(env->CallIntMethod(bridge_, activeSoundMask_));
        if (clearException(env)) {
            result = 0;
        }
    }
    detach(attached);
    return result;
}

bool AndroidAudioBackend::voiceAvailable() const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    const bool result = env && bridge_ &&
                        env->CallBooleanMethod(bridge_, voiceAvailable_) == JNI_TRUE &&
                        !clearException(env);
    detach(attached);
    return result;
}

bool AndroidAudioBackend::startVoice() {
    bool attached = false;
    JNIEnv *env = environment(attached);
    const bool result = env && bridge_ &&
                        env->CallBooleanMethod(bridge_, startVoice_) == JNI_TRUE &&
                        !clearException(env);
    detach(attached);
    return result;
}

void AndroidAudioBackend::stopVoice() {
    callVoid(stopVoice_);
}

std::optional<VoiceInputChunk> AndroidAudioBackend::takeVoiceInput() {
    bool attached = false;
    JNIEnv *env = environment(attached);
    std::optional<VoiceInputChunk> result;
    if (env && bridge_) {
        auto pcm = static_cast<jshortArray>(env->CallObjectMethod(bridge_, takeVoiceInput_));
        if (!clearException(env) && pcm) {
            const jsize length = env->GetArrayLength(pcm);
            if (length > 0) {
                VoiceInputChunk chunk;
                chunk.samples.resize(static_cast<std::size_t>(length));
                env->GetShortArrayRegion(pcm, 0, length,
                                         reinterpret_cast<jshort *>(chunk.samples.data()));
                chunk.sampleRate = env->CallIntMethod(bridge_, voiceInputSampleRate_);
                if (!clearException(env) && chunk.sampleRate > 0) {
                    result = std::move(chunk);
                }
            }
            env->DeleteLocalRef(pcm);
        }
    }
    detach(attached);
    return result;
}

bool AndroidAudioBackend::queueVoiceOutput(std::span<const int16_t> samples, int32_t sampleRate) {
    if (samples.empty() ||
        samples.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
        return false;
    }
    bool attached = false;
    JNIEnv *env = environment(attached);
    bool result = false;
    if (env && bridge_) {
        const auto length = static_cast<jsize>(samples.size());
        jshortArray pcm = env->NewShortArray(length);
        if (pcm) {
            env->SetShortArrayRegion(pcm, 0, length,
                                     reinterpret_cast<const jshort *>(samples.data()));
            if (!clearException(env)) {
                result = env->CallBooleanMethod(bridge_, queueVoiceOutput_, pcm, sampleRate) ==
                             JNI_TRUE &&
                         !clearException(env);
            }
            env->DeleteLocalRef(pcm);
        }
    }
    detach(attached);
    return result;
}

std::string AndroidAudioBackend::voiceDiagnostics() const {
    bool attached = false;
    JNIEnv *env = environment(attached);
    std::string result;
    if (env && bridge_) {
        auto value = static_cast<jstring>(env->CallObjectMethod(bridge_, voiceDiagnostics_));
        if (!clearException(env) && value) {
            const char *characters = env->GetStringUTFChars(value, nullptr);
            if (characters) {
                result = characters;
                env->ReleaseStringUTFChars(value, characters);
            } else {
                clearException(env);
            }
            env->DeleteLocalRef(value);
        }
    }
    detach(attached);
    return result;
}

void AndroidAudioBackend::shutdown() {
    if (!shutdownCalled_) {
        shutdownCalled_ = true;
        callVoid(shutdown_);
    }
}

} // namespace codboz
