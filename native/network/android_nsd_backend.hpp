#pragma once

#include "zeroconf_backend.hpp"

#include <jni.h>

namespace codboz {

class AndroidNsdBackend final : public ZeroConfBackend {
  public:
    AndroidNsdBackend(JNIEnv *env, jobject bridge);
    ~AndroidNsdBackend() override;

    AndroidNsdBackend(const AndroidNsdBackend &) = delete;
    AndroidNsdBackend &operator=(const AndroidNsdBackend &) = delete;

    bool startSearch(uint32_t handle, const std::string &serviceType,
                     const std::string &domain) override;
    void stopSearch(uint32_t handle) override;
    bool publish(uint32_t handle, uint16_t port, const std::string &name,
                 const std::string &serviceType, const std::string &domain,
                 const std::vector<std::string> &txtRecords) override;
    bool update(uint32_t handle, const std::vector<std::string> &txtRecords) override;
    void unpublish(uint32_t handle) override;
    void shutdown() override;

  private:
    JNIEnv *environment(bool &attached) const;
    void detach(bool attached) const;
    bool clearException(JNIEnv *env) const;
    jobjectArray stringArray(JNIEnv *env, const std::vector<std::string> &values) const;

    JavaVM *vm_ = nullptr;
    jobject bridge_ = nullptr;
    jmethodID startSearch_ = nullptr;
    jmethodID stopSearch_ = nullptr;
    jmethodID publish_ = nullptr;
    jmethodID update_ = nullptr;
    jmethodID unpublish_ = nullptr;
    jmethodID shutdown_ = nullptr;
    bool shutdownCalled_ = false;
};

} // namespace codboz
