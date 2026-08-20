#include "android_nsd_backend.hpp"

#include <stdexcept>

namespace codboz {

AndroidNsdBackend::AndroidNsdBackend(JNIEnv *env, jobject bridge) {
    if (!env || !bridge || env->GetJavaVM(&vm_) != JNI_OK) {
        throw std::runtime_error("invalid Android NSD bridge");
    }
    bridge_ = env->NewGlobalRef(bridge);
    if (!bridge_) {
        throw std::runtime_error("could not retain Android NSD bridge");
    }
    const jclass type = env->GetObjectClass(bridge_);
    startSearch_ =
        env->GetMethodID(type, "startSearch", "(ILjava/lang/String;Ljava/lang/String;)Z");
    stopSearch_ = env->GetMethodID(type, "stopSearch", "(I)V");
    publish_ = env->GetMethodID(
        type, "publish",
        "(IILjava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)Z");
    update_ = env->GetMethodID(type, "update", "(I[Ljava/lang/String;)Z");
    unpublish_ = env->GetMethodID(type, "unpublish", "(I)V");
    shutdown_ = env->GetMethodID(type, "shutdown", "()V");
    env->DeleteLocalRef(type);
    if (clearException(env) || !startSearch_ || !stopSearch_ || !publish_ || !update_ ||
        !unpublish_ || !shutdown_) {
        env->DeleteGlobalRef(bridge_);
        bridge_ = nullptr;
        throw std::runtime_error("incomplete Android NSD bridge");
    }
}

AndroidNsdBackend::~AndroidNsdBackend() {
    shutdown();
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->DeleteGlobalRef(bridge_);
    }
    bridge_ = nullptr;
    detach(attached);
}

JNIEnv *AndroidNsdBackend::environment(bool &attached) const {
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

void AndroidNsdBackend::detach(bool attached) const {
    if (attached && vm_) {
        vm_->DetachCurrentThread();
    }
}

bool AndroidNsdBackend::clearException(JNIEnv *env) const {
    if (!env || !env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

jobjectArray AndroidNsdBackend::stringArray(JNIEnv *env,
                                            const std::vector<std::string> &values) const {
    const jclass stringType = env->FindClass("java/lang/String");
    if (!stringType) {
        clearException(env);
        return nullptr;
    }
    jobjectArray result =
        env->NewObjectArray(static_cast<jsize>(values.size()), stringType, nullptr);
    env->DeleteLocalRef(stringType);
    if (!result) {
        clearException(env);
        return nullptr;
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        jstring value = env->NewStringUTF(values[index].c_str());
        if (!value) {
            clearException(env);
            env->DeleteLocalRef(result);
            return nullptr;
        }
        env->SetObjectArrayElement(result, static_cast<jsize>(index), value);
        env->DeleteLocalRef(value);
        if (clearException(env)) {
            env->DeleteLocalRef(result);
            return nullptr;
        }
    }
    return result;
}

bool AndroidNsdBackend::startSearch(uint32_t handle, const std::string &serviceType,
                                    const std::string &domain) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    bool result = false;
    if (env && bridge_) {
        jstring type = env->NewStringUTF(serviceType.c_str());
        jstring domainValue = env->NewStringUTF(domain.c_str());
        if (type && domainValue) {
            result = env->CallBooleanMethod(bridge_, startSearch_, static_cast<jint>(handle), type,
                                            domainValue) == JNI_TRUE &&
                     !clearException(env);
        } else {
            clearException(env);
        }
        if (type)
            env->DeleteLocalRef(type);
        if (domainValue)
            env->DeleteLocalRef(domainValue);
    }
    detach(attached);
    return result;
}

void AndroidNsdBackend::stopSearch(uint32_t handle) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, stopSearch_, static_cast<jint>(handle));
        clearException(env);
    }
    detach(attached);
}

bool AndroidNsdBackend::publish(uint32_t handle, uint16_t port, const std::string &name,
                                const std::string &serviceType, const std::string &domain,
                                const std::vector<std::string> &txtRecords) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    bool result = false;
    if (env && bridge_) {
        jstring nameValue = env->NewStringUTF(name.c_str());
        jstring type = env->NewStringUTF(serviceType.c_str());
        jstring domainValue = env->NewStringUTF(domain.c_str());
        jobjectArray txt = stringArray(env, txtRecords);
        if (nameValue && type && domainValue && txt) {
            result = env->CallBooleanMethod(bridge_, publish_, static_cast<jint>(handle),
                                            static_cast<jint>(port), nameValue, type, domainValue,
                                            txt) == JNI_TRUE &&
                     !clearException(env);
        } else {
            clearException(env);
        }
        if (nameValue)
            env->DeleteLocalRef(nameValue);
        if (type)
            env->DeleteLocalRef(type);
        if (domainValue)
            env->DeleteLocalRef(domainValue);
        if (txt)
            env->DeleteLocalRef(txt);
    }
    detach(attached);
    return result;
}

bool AndroidNsdBackend::update(uint32_t handle, const std::vector<std::string> &txtRecords) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    bool result = false;
    if (env && bridge_) {
        jobjectArray txt = stringArray(env, txtRecords);
        if (txt) {
            result = env->CallBooleanMethod(bridge_, update_, static_cast<jint>(handle), txt) ==
                         JNI_TRUE &&
                     !clearException(env);
            env->DeleteLocalRef(txt);
        }
    }
    detach(attached);
    return result;
}

void AndroidNsdBackend::unpublish(uint32_t handle) {
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, unpublish_, static_cast<jint>(handle));
        clearException(env);
    }
    detach(attached);
}

void AndroidNsdBackend::shutdown() {
    if (shutdownCalled_) {
        return;
    }
    shutdownCalled_ = true;
    bool attached = false;
    JNIEnv *env = environment(attached);
    if (env && bridge_) {
        env->CallVoidMethod(bridge_, shutdown_);
        clearException(env);
    }
    detach(attached);
}

} // namespace codboz
