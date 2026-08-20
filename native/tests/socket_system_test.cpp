#include "network/socket_system.hpp"

#include "test_macros.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

int main() {
    using namespace codboz;

    uint32_t loopback = 0;
    CHECK(SocketSystem::parseIpv4("127.0.0.1", loopback));
    CHECK(SocketSystem::formatIpv4(loopback) == "127.0.0.1");
    CHECK(SocketSystem::networkToHost16(SocketSystem::hostToNetwork16(3074)) == 3074);

    SocketSystem sockets;
    const uint32_t receiver = sockets.create(1, 2);
    const uint32_t sender = sockets.create(1, 2);
    CHECK(receiver == 3000);
    CHECK(sender == 3001);

    guest::InetAddress address{};
    address.type = 1;
    address.ip_address = loopback;
    const int32_t bindResult = sockets.bind(receiver, &address, true);
    if (bindResult != 0)
        std::fprintf(stderr, "bind error=%d\n", sockets.error());
    CHECK(bindResult == 0);
    CHECK(sockets.localName(receiver, address) == 0);
    CHECK(address.port != 0);

    constexpr std::array<uint8_t, 5> message{'h', 'e', 'l', 'l', 'o'};
    CHECK(sockets.sendTo(sender, message, 0, address) == static_cast<int32_t>(message.size()));
    SocketSystem::ReceiveResult received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        received = sockets.receive(receiver, 32, 0, true);
        if (received.count < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (received.count < 0 && std::chrono::steady_clock::now() < deadline);
    CHECK(received.count == static_cast<int32_t>(message.size()));
    CHECK(received.data.size() == message.size());
    CHECK(std::memcmp(received.data.data(), message.data(), message.size()) == 0);
    CHECK(received.source.has_value());

    CHECK(sockets.close(sender) == 0);
    CHECK(sockets.close(receiver) == 0);
    CHECK(sockets.close(receiver) == 1);

    CHECK(sockets.startLookup("127.0.0.1", SocketSystem::hostToNetwork16(3074)));
    std::optional<SocketSystem::LookupResult> lookup;
    const auto lookupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        lookup = sockets.pollLookup();
        if (!lookup)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!lookup && std::chrono::steady_clock::now() < lookupDeadline);
    CHECK(lookup.has_value());
    CHECK(lookup->result == 0);
    CHECK(lookup->address.ip_address == loopback);
    CHECK(lookup->address.port == SocketSystem::hostToNetwork16(3074));

    CHECK(sockets.startLookup("", SocketSystem::hostToNetwork16(3478)));
    lookup.reset();
    const auto emptyLookupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        lookup = sockets.pollLookup();
        if (!lookup)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!lookup && std::chrono::steady_clock::now() < emptyLookupDeadline);
    CHECK(lookup.has_value());
    CHECK(lookup->result == 1);
    CHECK(lookup->error == 1013);
    CHECK(lookup->address.port == SocketSystem::hostToNetwork16(3478));

    SocketSystem failover([&](const std::string &hostname, uint16_t port) {
        CHECK(hostname == "multi.test");
        guest::InetAddress first{};
        first.type = 4;
        first.port = port;
        guest::InetAddress second{};
        second.type = 1;
        second.ip_address = loopback;
        second.port = port;
        return std::vector<guest::InetAddress>{first, second};
    });

    const uint32_t listener = failover.create(0, 2);
    guest::InetAddress listenerAddress{};
    listenerAddress.type = 1;
    listenerAddress.ip_address = loopback;
    CHECK(failover.bind(listener, &listenerAddress, true) == 0);
    CHECK(failover.localName(listener, listenerAddress) == 0);
    CHECK(failover.listen(listener, 1) == 0);

    guest::InetAddress resolved{};
    resolved.port = listenerAddress.port;
    CHECK(failover.lookup("multi.test", resolved) == 0);
    CHECK(resolved.type == 4);
    const uint32_t client = failover.create(0, 2);
    CHECK(failover.connect(client, resolved, {0x1234u, 0x5678u}) == 0);

    std::optional<SocketSystem::Event> connected;
    const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        for (const auto &event : failover.poll()) {
            if (event.type == SocketSystem::EventType::Connect) {
                connected = event;
            }
        }
        if (!connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } while (!connected && std::chrono::steady_clock::now() < connectDeadline);
    CHECK(connected.has_value());
    CHECK(connected->callback.function == 0x1234u);
    CHECK(connected->callback.userData == 0x5678u);
    CHECK(connected->result == 0);
    guest::InetAddress peer{};
    CHECK(failover.peerName(client, peer) == 0);
    CHECK(peer.ip_address == loopback);
    CHECK(failover.close(client) == 0);
    CHECK(failover.close(listener) == 0);

    struct ResolverGate {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool released = false;
    };
    auto gate = std::make_shared<ResolverGate>();
    SocketSystem interruptible([gate](const std::string &, uint16_t) {
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->condition.notify_all();
        gate->condition.wait(lock, [gate] { return gate->released; });
        return std::vector<guest::InetAddress>{};
    });
    guest::InetAddress interruptedAddress{};
    const auto interruptStart = std::chrono::steady_clock::now();
    CHECK(interruptible.lookup("blocked.test", interruptedAddress, [gate] {
        std::lock_guard lock(gate->mutex);
        return gate->entered;
    }) == 1);
    CHECK(std::chrono::steady_clock::now() - interruptStart < std::chrono::milliseconds(500));
    {
        std::lock_guard lock(gate->mutex);
        gate->released = true;
    }
    gate->condition.notify_all();

    auto asyncGate = std::make_shared<ResolverGate>();
    const auto destroyStart = std::chrono::steady_clock::now();
    {
        SocketSystem asynchronous([asyncGate](const std::string &, uint16_t) {
            std::unique_lock lock(asyncGate->mutex);
            asyncGate->entered = true;
            asyncGate->condition.notify_all();
            asyncGate->condition.wait(lock, [asyncGate] { return asyncGate->released; });
            return std::vector<guest::InetAddress>{};
        });
        CHECK(asynchronous.startLookup("blocked.test", 0));
        std::unique_lock lock(asyncGate->mutex);
        CHECK(asyncGate->condition.wait_for(lock, std::chrono::milliseconds(500),
                                            [asyncGate] { return asyncGate->entered; }));
        lock.unlock();
        asynchronous.cancelLookup();
    }
    CHECK(std::chrono::steady_clock::now() - destroyStart < std::chrono::milliseconds(500));
    {
        std::lock_guard lock(asyncGate->mutex);
        asyncGate->released = true;
    }
    asyncGate->condition.notify_all();
    return 0;
}
