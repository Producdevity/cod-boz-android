#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_structs.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codboz {

class SocketSystem {
  public:
    struct Callback {
        GuestAddr function = 0;
        GuestAddr userData = 0;
    };

    enum class EventType {
        Connect,
        Accept,
        Readable,
        Writable,
    };

    struct Event {
        EventType type;
        uint32_t handle;
        Callback callback;
        int32_t result = 0;
    };

    struct ReceiveResult {
        int32_t count = -1;
        std::vector<uint8_t> data;
        std::optional<guest::InetAddress> source;
    };

    struct LookupResult {
        int32_t result = 1;
        int32_t error = 1013;
        guest::InetAddress address{};
        std::vector<guest::InetAddress> addresses;
    };

    using Resolver = std::function<std::vector<guest::InetAddress>(const std::string &, uint16_t)>;

    explicit SocketSystem(Resolver resolver = {});
    ~SocketSystem();

    SocketSystem(const SocketSystem &) = delete;
    SocketSystem &operator=(const SocketSystem &) = delete;

    uint32_t create(uint32_t type, int32_t domain);
    int32_t close(uint32_t handle);
    int32_t bind(uint32_t handle, const guest::InetAddress *address, bool reuseAddress);
    int32_t listen(uint32_t handle, uint16_t backlog);
    uint32_t accept(uint32_t handle, guest::InetAddress *address, Callback callback);
    int32_t connect(uint32_t handle, const guest::InetAddress &address, Callback callback);
    int32_t send(uint32_t handle, std::span<const uint8_t> data, int32_t flags);
    int32_t sendTo(uint32_t handle, std::span<const uint8_t> data, int32_t flags,
                   const guest::InetAddress &address);
    ReceiveResult receive(uint32_t handle, uint32_t length, int32_t flags, bool withSource);
    int32_t readable(uint32_t handle, Callback callback);
    int32_t writable(uint32_t handle, Callback callback);
    std::vector<Event> poll();

    int32_t getInt(uint32_t key);
    int32_t error() const;
    std::string getString(uint32_t key);
    int32_t localName(uint32_t handle, guest::InetAddress &address);
    int32_t peerName(uint32_t handle, guest::InetAddress &address);
    int32_t lookup(const std::string &hostname, guest::InetAddress &address,
                   const std::function<bool()> &interrupted = {});
    bool startLookup(const std::string &hostname, uint16_t port);
    std::optional<LookupResult> pollLookup();
    void cancelLookup();
    void setError(int32_t value);

    static uint32_t hostToNetwork32(uint32_t value);
    static uint32_t networkToHost32(uint32_t value);
    static uint16_t hostToNetwork16(uint16_t value);
    static uint16_t networkToHost16(uint16_t value);
    static bool parseIpv4(const std::string &text, uint32_t &address);
    static std::string formatIpv4(uint32_t address);
    static std::string formatAddress(const guest::InetAddress &address, bool includePort);

  private:
    struct Slot {
        int fd = -1;
        uint32_t generation = 0;
        int32_t type = 0;
        int32_t domain = 0;
        Callback connectCallback;
        Callback acceptCallback;
        Callback readableCallback;
        Callback writableCallback;
        bool connectPending = false;
        bool inUse = false;
        std::vector<guest::InetAddress> connectCandidates;
        std::size_t connectCandidate = 0;
    };

    struct LookupTask {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<LookupResult> result;
        std::atomic<bool> cancelled = false;
    };

    Slot *resolve(uint32_t handle);
    const Slot *resolve(uint32_t handle) const;
    int allocate(int fd, int32_t type, int32_t domain);
    int createFd(int32_t type, int32_t domain);
    int connectCandidate(Slot &slot, std::size_t index);
    int advanceConnection(Slot &slot, int nativeError);
    void release(std::size_t index);
    int32_t fail(int nativeError);
    std::array<Slot, 32> slots_{};
    int32_t error_ = 0;
    std::shared_ptr<LookupTask> lookup_;
    Resolver resolver_;
    std::deque<std::vector<guest::InetAddress>> resolvedCandidates_;
};

} // namespace codboz
