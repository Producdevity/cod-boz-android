#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codboz {

class ZeroConfBackend {
  public:
    virtual ~ZeroConfBackend() = default;

    virtual bool startSearch(uint32_t handle, const std::string &serviceType,
                             const std::string &domain) = 0;
    virtual void stopSearch(uint32_t handle) = 0;
    virtual bool publish(uint32_t handle, uint16_t port, const std::string &name,
                         const std::string &serviceType, const std::string &domain,
                         const std::vector<std::string> &txtRecords) = 0;
    virtual bool update(uint32_t handle, const std::vector<std::string> &txtRecords) = 0;
    virtual void unpublish(uint32_t handle) = 0;
    virtual void shutdown() = 0;
};

} // namespace codboz
