#include "socket_system.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <utility>

namespace codboz {
namespace {

constexpr uint32_t kFirstHandle = 3000;
constexpr int32_t kSuccess = 0;
constexpr int32_t kFailure = 1;

int32_t mapError(int value) {
    switch (value) {
    case 0:
        return 0;
    case EPERM:
        return 1014;
    case EBADF:
    case EINVAL:
    case EADDRNOTAVAIL:
        return 1;
    case EAGAIN:
        return 1000;
    case EACCES:
        return 5;
    case EPIPE:
        return 1010;
    case ENOTSOCK:
        return 1003;
    case EMSGSIZE:
        return 1004;
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT:
        return 7;
    case EADDRINUSE:
        return 1005;
    case ENETUNREACH:
    case ENETDOWN:
    case EHOSTUNREACH:
        return 1006;
    case ECONNABORTED:
    case ECONNRESET:
        return 1007;
    case EISCONN:
        return 1008;
    case ENOTCONN:
        return 1009;
    case ETIMEDOUT:
        return 1011;
    case ECONNREFUSED:
        return 1012;
    case EALREADY:
        return 1002;
    case EINPROGRESS:
        return 1001;
    case EMFILE:
    case ENFILE:
        return 2;
    default:
        return 5;
    }
}

int familyFor(int32_t domain) {
    if (domain == 0 || domain == 2)
        return AF_INET;
    if (domain == 30)
        return AF_INET6;
    return -1;
}

int kindFor(uint32_t type) {
    if (type == 0)
        return SOCK_STREAM;
    if (type == 1)
        return SOCK_DGRAM;
    if (type == 2)
        return SOCK_RAW;
    return -1;
}

int nativeFlags(int32_t flags, bool sending) {
    int result = 0;
#ifdef MSG_MORE
    if ((flags & 1) != 0)
        result |= MSG_MORE;
#else
    (void)flags;
#endif
#ifdef MSG_NOSIGNAL
    if (sending)
        result |= MSG_NOSIGNAL;
#else
    (void)sending;
#endif
    return result;
}

bool toSockaddr(const guest::InetAddress &address, sockaddr_storage &storage, socklen_t &length) {
    std::memset(&storage, 0, sizeof(storage));
    if ((address.type & 2) != 0) {
        auto *value = reinterpret_cast<sockaddr_in6 *>(&storage);
        value->sin6_family = AF_INET6;
        value->sin6_port = address.port;
        std::memcpy(&value->sin6_addr, address.ipv6_address, sizeof(value->sin6_addr));
        length = sizeof(*value);
        return true;
    }
    if ((address.type & 4) != 0)
        return false;
    auto *value = reinterpret_cast<sockaddr_in *>(&storage);
    value->sin_family = AF_INET;
    value->sin_port = address.port;
    value->sin_addr.s_addr = address.ip_address;
    length = sizeof(*value);
    return true;
}

guest::InetAddress fromSockaddr(const sockaddr *source, socklen_t length) {
    guest::InetAddress result{};
    if (source && source->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto *value = reinterpret_cast<const sockaddr_in *>(source);
        result.type = 1;
        result.ip_address = value->sin_addr.s_addr;
        result.port = value->sin_port;
        inet_ntop(AF_INET, &value->sin_addr, result.string, sizeof(result.string));
    } else if (source && source->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto *value = reinterpret_cast<const sockaddr_in6 *>(source);
        result.type = 2;
        std::memcpy(result.ipv6_address, &value->sin6_addr, sizeof(result.ipv6_address));
        result.port = value->sin6_port;
        inet_ntop(AF_INET6, &value->sin6_addr, result.string, sizeof(result.string));
    }
    return result;
}

std::vector<guest::InetAddress> resolveIpv4(const std::string &hostname, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *results = nullptr;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &results) != 0 || !results) {
        if (results) {
            freeaddrinfo(results);
        }
        return {};
    }

    std::vector<guest::InetAddress> addresses;
    for (const addrinfo *entry = results; entry; entry = entry->ai_next) {
        auto address = fromSockaddr(entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen));
        address.port = port;
        if (address.type != 1) {
            continue;
        }
        const auto duplicate =
            std::find_if(addresses.begin(), addresses.end(), [&](const auto &existing) {
                return existing.ip_address == address.ip_address && existing.port == address.port;
            });
        if (duplicate == addresses.end()) {
            addresses.push_back(address);
        }
    }
    freeaddrinfo(results);
    return addresses;
}

} // namespace

SocketSystem::SocketSystem(Resolver resolver)
    : resolver_(resolver ? std::move(resolver) : Resolver(resolveIpv4)) {}

SocketSystem::~SocketSystem() {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].inUse)
            release(i);
    }
}

void SocketSystem::setError(int32_t value) {
    error_ = value;
}

int32_t SocketSystem::fail(int nativeError) {
    setError(mapError(nativeError));
    return kFailure;
}

SocketSystem::Slot *SocketSystem::resolve(uint32_t handle) {
    if (handle < kFirstHandle || handle >= kFirstHandle + slots_.size()) {
        setError(1);
        return nullptr;
    }
    Slot &slot = slots_[handle - kFirstHandle];
    if (!slot.inUse) {
        setError(1);
        return nullptr;
    }
    return &slot;
}

const SocketSystem::Slot *SocketSystem::resolve(uint32_t handle) const {
    if (handle < kFirstHandle || handle >= kFirstHandle + slots_.size())
        return nullptr;
    const Slot &slot = slots_[handle - kFirstHandle];
    return slot.inUse ? &slot : nullptr;
}

int SocketSystem::allocate(int fd, int32_t type, int32_t domain) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].inUse)
            continue;
        uint32_t generation = slots_[i].generation + 1u;
        if (generation == 0)
            generation = 1;
        slots_[i] = {};
        slots_[i].fd = fd;
        slots_[i].generation = generation;
        slots_[i].type = type;
        slots_[i].domain = domain;
        slots_[i].inUse = true;
        return static_cast<int>(i);
    }
    setError(2);
    return -1;
}

void SocketSystem::release(std::size_t index) {
    Slot &slot = slots_[index];
    uint32_t generation = slot.generation + 1u;
    if (generation == 0)
        generation = 1;
    if (slot.fd >= 0)
        ::close(slot.fd);
    slot = {};
    slot.fd = -1;
    slot.generation = generation;
}

uint32_t SocketSystem::create(uint32_t type, int32_t domain) {
    const int fd = createFd(static_cast<int32_t>(type), domain);
    if (fd < 0) {
        return 0;
    }
    const int index = allocate(fd, static_cast<int32_t>(type), domain);
    if (index < 0) {
        ::close(fd);
        return 0;
    }
    setError(0);
    return kFirstHandle + static_cast<uint32_t>(index);
}

int SocketSystem::createFd(int32_t type, int32_t domain) {
    const int family = familyFor(domain);
    const int kind = kindFor(static_cast<uint32_t>(type));
    if (family < 0 || kind < 0) {
        errno = EAFNOSUPPORT;
        setError(7);
        return -1;
    }
    const int fd = ::socket(family, kind, 0);
    if (fd < 0) {
        setError(mapError(errno));
        return -1;
    }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int nativeError = errno;
        ::close(fd);
        setError(mapError(nativeError));
        return -1;
    }
    if (type == 1) {
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    }
    return fd;
}

int32_t SocketSystem::close(uint32_t handle) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    release(static_cast<std::size_t>(slot - slots_.data()));
    setError(0);
    return kSuccess;
}

int32_t SocketSystem::bind(uint32_t handle, const guest::InetAddress *address, bool reuseAddress) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    guest::InetAddress any{};
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (!toSockaddr(address ? *address : any, storage, length)) {
        setError(7);
        return kFailure;
    }
    if (reuseAddress) {
        int enabled = 1;
        if (setsockopt(slot->fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
            return fail(errno);
        }
    }
    if (::bind(slot->fd, reinterpret_cast<const sockaddr *>(&storage), length) < 0) {
        return fail(errno);
    }
    setError(0);
    return kSuccess;
}

int32_t SocketSystem::listen(uint32_t handle, uint16_t backlog) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    if (::listen(slot->fd, backlog) < 0)
        return fail(errno);
    setError(0);
    return kSuccess;
}

uint32_t SocketSystem::accept(uint32_t handle, guest::InetAddress *address, Callback callback) {
    Slot *slot = resolve(handle);
    if (!slot)
        return 0;
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    const int fd = ::accept(slot->fd, reinterpret_cast<sockaddr *>(&peer), &length);
    if (fd < 0) {
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && callback.function) {
            slot->acceptCallback = callback;
            slot->readableCallback = {};
        }
        setError(mapError(errno));
        return 0;
    }
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int nativeError = errno;
        ::close(fd);
        setError(mapError(nativeError));
        return 0;
    }
    const int index = allocate(fd, 0, slot->domain);
    if (index < 0) {
        ::close(fd);
        return 0;
    }
    slot->acceptCallback = {};
    if (address)
        *address = fromSockaddr(reinterpret_cast<sockaddr *>(&peer), length);
    setError(0);
    return kFirstHandle + static_cast<uint32_t>(index);
}

int32_t SocketSystem::connect(uint32_t handle, const guest::InetAddress &address,
                              Callback callback) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    slot->connectCandidates = {address};
    if (slot->type == 0) {
        const auto resolved = std::find_if(
            resolvedCandidates_.begin(), resolvedCandidates_.end(), [&](const auto &candidates) {
                return !candidates.empty() && candidates.front().type == address.type &&
                       candidates.front().ip_address == address.ip_address &&
                       candidates.front().port == address.port;
            });
        if (resolved != resolvedCandidates_.end()) {
            slot->connectCandidates = std::move(*resolved);
            resolvedCandidates_.erase(resolved);
        }
    }
    slot->connectCandidate = 0;
    slot->connectCallback = callback;
    const int nativeError = connectCandidate(*slot, 0);
    return nativeError == 0 || nativeError == EINPROGRESS || nativeError == EALREADY
               ? kSuccess
               : fail(nativeError);
}

int SocketSystem::connectCandidate(Slot &slot, std::size_t index) {
    if (index >= slot.connectCandidates.size()) {
        slot.connectPending = false;
        return ECONNREFUSED;
    }
    slot.connectCandidate = index;
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (!toSockaddr(slot.connectCandidates[index], storage, length)) {
        return advanceConnection(slot, EAFNOSUPPORT);
    }
    if (::connect(slot.fd, reinterpret_cast<const sockaddr *>(&storage), length) == 0) {
        slot.connectPending = slot.connectCallback.function != 0;
        setError(0);
        return 0;
    }
    const int nativeError = errno;
    if (nativeError == EINPROGRESS || nativeError == EALREADY) {
        slot.connectPending = true;
        setError(1001);
        return nativeError;
    }
    return advanceConnection(slot, nativeError);
}

int SocketSystem::advanceConnection(Slot &slot, int nativeError) {
    while (slot.connectCandidate + 1u < slot.connectCandidates.size()) {
        ++slot.connectCandidate;
        if (slot.fd >= 0) {
            ::close(slot.fd);
        }
        slot.fd = createFd(slot.type, slot.domain);
        if (slot.fd < 0) {
            return errno;
        }
        sockaddr_storage storage{};
        socklen_t length = 0;
        if (!toSockaddr(slot.connectCandidates[slot.connectCandidate], storage, length)) {
            nativeError = EAFNOSUPPORT;
            continue;
        }
        if (::connect(slot.fd, reinterpret_cast<const sockaddr *>(&storage), length) == 0) {
            slot.connectPending = slot.connectCallback.function != 0;
            setError(0);
            return 0;
        }
        nativeError = errno;
        if (nativeError == EINPROGRESS || nativeError == EALREADY) {
            slot.connectPending = true;
            setError(1001);
            return nativeError;
        }
    }
    slot.connectPending = false;
    return nativeError;
}

int32_t SocketSystem::send(uint32_t handle, std::span<const uint8_t> data, int32_t flags) {
    Slot *slot = resolve(handle);
    if (!slot)
        return -1;
    const ssize_t count = ::send(slot->fd, data.data(), data.size(), nativeFlags(flags, true));
    if (count < 0) {
        setError(mapError(errno));
        return -1;
    }
    setError(0);
    return static_cast<int32_t>(count);
}

int32_t SocketSystem::sendTo(uint32_t handle, std::span<const uint8_t> data, int32_t flags,
                             const guest::InetAddress &address) {
    Slot *slot = resolve(handle);
    if (!slot)
        return -1;
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (!toSockaddr(address, storage, length)) {
        setError(7);
        return -1;
    }
    const ssize_t count = ::sendto(slot->fd, data.data(), data.size(), nativeFlags(flags, true),
                                   reinterpret_cast<const sockaddr *>(&storage), length);
    if (count < 0) {
        setError(mapError(errno));
        return -1;
    }
    setError(0);
    return static_cast<int32_t>(count);
}

SocketSystem::ReceiveResult SocketSystem::receive(uint32_t handle, uint32_t length, int32_t flags,
                                                  bool withSource) {
    ReceiveResult result;
    Slot *slot = resolve(handle);
    if (!slot)
        return result;
    result.data.resize(length);
    sockaddr_storage source{};
    socklen_t sourceLength = sizeof(source);
    int options = nativeFlags(flags, false);
#ifdef MSG_TRUNC
    if (withSource)
        options |= MSG_TRUNC;
#endif
    const ssize_t count =
        withSource ? ::recvfrom(slot->fd, result.data.data(), result.data.size(), options,
                                reinterpret_cast<sockaddr *>(&source), &sourceLength)
                   : ::recv(slot->fd, result.data.data(), result.data.size(), options);
    if (count < 0) {
        setError(mapError(errno));
        result.data.clear();
        return result;
    }
    result.count = static_cast<int32_t>(count);
    result.data.resize(std::min<std::size_t>(result.data.size(), static_cast<std::size_t>(count)));
    if (withSource) {
        result.source = fromSockaddr(reinterpret_cast<sockaddr *>(&source), sourceLength);
    }
    setError(0);
    return result;
}

int32_t SocketSystem::readable(uint32_t handle, Callback callback) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    slot->readableCallback = callback;
    slot->acceptCallback = {};
    setError(0);
    return kSuccess;
}

int32_t SocketSystem::writable(uint32_t handle, Callback callback) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    slot->writableCallback = callback;
    setError(0);
    return kSuccess;
}

std::vector<SocketSystem::Event> SocketSystem::poll() {
    struct PollEntry {
        pollfd value{};
        std::size_t index = 0;
        uint32_t generation = 0;
    };
    std::vector<PollEntry> entries;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot &slot = slots_[i];
        if (!slot.inUse)
            continue;
        short events = 0;
        if (slot.acceptCallback.function || slot.readableCallback.function)
            events |= POLLIN;
        if (slot.connectPending || slot.writableCallback.function)
            events |= POLLOUT;
        if (events)
            entries.push_back({{slot.fd, events, 0}, i, slot.generation});
    }
    if (entries.empty())
        return {};
    std::vector<pollfd> native;
    native.reserve(entries.size());
    for (const auto &entry : entries)
        native.push_back(entry.value);
    if (::poll(native.data(), native.size(), 0) <= 0)
        return {};

    std::vector<Event> result;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!native[i].revents)
            continue;
        const auto index = entries[i].index;
        Slot &slot = slots_[index];
        if (!slot.inUse || slot.generation != entries[i].generation)
            continue;
        const uint32_t handle = kFirstHandle + static_cast<uint32_t>(index);
        const short events = native[i].revents;
        if (slot.connectPending && (events & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
            int nativeError = 0;
            socklen_t length = sizeof(nativeError);
            if (getsockopt(slot.fd, SOL_SOCKET, SO_ERROR, &nativeError, &length) < 0) {
                nativeError = errno;
            }
            if (nativeError != 0 && slot.connectCandidate + 1u < slot.connectCandidates.size()) {
                nativeError = advanceConnection(slot, nativeError);
                if (nativeError == EINPROGRESS || nativeError == EALREADY) {
                    continue;
                }
            }
            result.push_back(
                {EventType::Connect, handle, slot.connectCallback, nativeError == 0 ? 0 : 1});
            setError(mapError(nativeError));
            slot.connectPending = false;
            slot.connectCallback = {};
        }
        if (slot.acceptCallback.function && (events & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            result.push_back({EventType::Accept, handle, slot.acceptCallback, 0});
            slot.acceptCallback = {};
        }
        if (slot.readableCallback.function && (events & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            result.push_back({EventType::Readable, handle, slot.readableCallback, 0});
            slot.readableCallback = {};
        }
        if (slot.writableCallback.function && (events & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
            result.push_back({EventType::Writable, handle, slot.writableCallback, 0});
            slot.writableCallback = {};
        }
    }
    return result;
}

int32_t SocketSystem::getInt(uint32_t key) {
    int32_t result = -1;
    if (key == 0)
        result = 32;
    if (key == 1 || key == 6)
        result = 1;
    if (key == 2)
        result = 3;
    if (result >= 0) {
        setError(0);
        return result;
    }
    setError(1);
    return -1;
}

int32_t SocketSystem::error() const {
    return error_;
}

std::string SocketSystem::getString(uint32_t key) {
    char hostname[256]{};
    if (key == 5) {
        setError(0);
        return {};
    }
    if (key != 3 && key != 4) {
        setError(1);
        return {};
    }
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = 0;
    setError(0);
    if (key == 4)
        return hostname;
    const char *dot = std::strchr(hostname, '.');
    return dot ? dot + 1 : "";
}

int32_t SocketSystem::localName(uint32_t handle, guest::InetAddress &address) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    sockaddr_storage value{};
    socklen_t length = sizeof(value);
    if (getsockname(slot->fd, reinterpret_cast<sockaddr *>(&value), &length) < 0) {
        return fail(errno);
    }
    address = fromSockaddr(reinterpret_cast<sockaddr *>(&value), length);
    setError(0);
    return kSuccess;
}

int32_t SocketSystem::peerName(uint32_t handle, guest::InetAddress &address) {
    Slot *slot = resolve(handle);
    if (!slot)
        return kFailure;
    sockaddr_storage value{};
    socklen_t length = sizeof(value);
    if (getpeername(slot->fd, reinterpret_cast<sockaddr *>(&value), &length) < 0) {
        return fail(errno);
    }
    address = fromSockaddr(reinterpret_cast<sockaddr *>(&value), length);
    setError(0);
    return kSuccess;
}

int32_t SocketSystem::lookup(const std::string &hostname, guest::InetAddress &address,
                             const std::function<bool()> &interrupted) {
    const uint16_t port = address.port ? address.port : hostToNetwork16(80);
    auto task = std::make_shared<LookupTask>();
    const Resolver resolver = resolver_;
    std::thread([task, hostname, port, resolver] {
        LookupResult result;
        result.address.port = port;
        try {
            result.addresses = resolver(hostname, port);
            if (!result.addresses.empty()) {
                result.address = result.addresses.front();
                result.result = kSuccess;
                result.error = 0;
            }
        } catch (...) {
        }
        {
            std::lock_guard lock(task->mutex);
            task->result = std::move(result);
        }
        task->condition.notify_all();
    }).detach();

    std::unique_lock lock(task->mutex);
    while (!task->result) {
        if (interrupted && interrupted()) {
            task->cancelled.store(true, std::memory_order_release);
            setError(1013);
            return kFailure;
        }
        task->condition.wait_for(lock, std::chrono::milliseconds(50));
    }
    auto result = std::move(*task->result);
    if (result.result != kSuccess) {
        setError(1013);
        return kFailure;
    }
    address = result.address;
    resolvedCandidates_.push_back(std::move(result.addresses));
    if (resolvedCandidates_.size() > 8) {
        resolvedCandidates_.pop_front();
    }
    setError(0);
    return kSuccess;
}

bool SocketSystem::startLookup(const std::string &hostname, uint16_t port) {
    if (lookup_) {
        setError(1002);
        return false;
    }
    auto task = std::make_shared<LookupTask>();
    lookup_ = task;
    const Resolver resolver = resolver_;
    std::thread([task, hostname, port, resolver] {
        LookupResult result;
        result.address.port = port;
        try {
            if (!hostname.empty()) {
                result.addresses = resolver(hostname, port);
            }
            if (!result.addresses.empty()) {
                result.address = result.addresses.front();
                result.result = kSuccess;
                result.error = 0;
            }
        } catch (...) {
        }
        {
            std::lock_guard lock(task->mutex);
            task->result = std::move(result);
        }
        task->condition.notify_all();
    }).detach();
    setError(0);
    return true;
}

std::optional<SocketSystem::LookupResult> SocketSystem::pollLookup() {
    auto task = lookup_;
    if (!task) {
        return std::nullopt;
    }
    std::unique_lock lock(task->mutex);
    if (!task->result) {
        return std::nullopt;
    }
    LookupResult result = std::move(*task->result);
    lock.unlock();
    lookup_.reset();
    if (task->cancelled.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    if (result.result == kSuccess) {
        resolvedCandidates_.push_back(result.addresses);
        if (resolvedCandidates_.size() > 8) {
            resolvedCandidates_.pop_front();
        }
    }
    setError(result.error);
    return result;
}

void SocketSystem::cancelLookup() {
    if (lookup_) {
        lookup_->cancelled.store(true, std::memory_order_release);
        lookup_.reset();
    }
}

uint32_t SocketSystem::hostToNetwork32(uint32_t value) {
    return htonl(value);
}
uint32_t SocketSystem::networkToHost32(uint32_t value) {
    return ntohl(value);
}
uint16_t SocketSystem::hostToNetwork16(uint16_t value) {
    return htons(value);
}
uint16_t SocketSystem::networkToHost16(uint16_t value) {
    return ntohs(value);
}

bool SocketSystem::parseIpv4(const std::string &text, uint32_t &address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, text.c_str(), &parsed) != 1)
        return false;
    address = parsed.s_addr;
    return true;
}

std::string SocketSystem::formatIpv4(uint32_t address) {
    in_addr parsed{address};
    char text[INET_ADDRSTRLEN]{};
    return inet_ntop(AF_INET, &parsed, text, sizeof(text)) ? text : std::string();
}

std::string SocketSystem::formatAddress(const guest::InetAddress &address, bool includePort) {
    char text[INET6_ADDRSTRLEN]{};
    const bool ipv6 = (address.type & 2) != 0;
    const void *source = ipv6 ? static_cast<const void *>(address.ipv6_address)
                              : static_cast<const void *>(&address.ip_address);
    if (!inet_ntop(ipv6 ? AF_INET6 : AF_INET, source, text, sizeof(text)))
        return {};
    if (!includePort)
        return text;
    const auto port = networkToHost16(address.port);
    return ipv6 ? "[" + std::string(text) + "]:" + std::to_string(port)
                : std::string(text) + ":" + std::to_string(port);
}

} // namespace codboz
