#include "dispatcher.hpp"

#include "audio/audio_backend.hpp"
#include "bridge/import_inventory.hpp"

#ifdef __ANDROID__
#include <android/log.h>
#include <android/native_window.h>
#endif

#include "cpu/dynarmic_cpu.hpp"
#include "game/codboz_frame_interpolation.hpp"
#include "game/codboz_player_name.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_call.hpp"
#include "guest/guest_structs.hpp"
#include "loader/guest_loader.hpp"
#include "loader/s3e_image.hpp"
#include "loader/source_profile.hpp"
#include "network/socket_system.hpp"
#include "s3e/display.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <format>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace codboz {

namespace {
// Extension interface hashes returned via s3eExtGetHash.
constexpr uint32_t kHashTouchpad = extensionHash("s3eTouchpad");
constexpr uint32_t kHashZeroConf = extensionHash("s3eZeroConf");
constexpr uint32_t kHashAudioUnit = extensionHash("IsAudioUnit");
constexpr uint32_t kHashIsDevice = extensionHash("IsDevice");
static_assert(kHashTouchpad != 0);
static_assert(kHashZeroConf != 0);
static_assert(kHashAudioUnit != 0);
static_assert(kHashIsDevice != 0);
constexpr int32_t kXperiaTouchpadWidth = 960;
constexpr int32_t kXperiaTouchpadHeight = 544;
constexpr float kXperiaAxisDeadzone = 6000.0f / 32767.0f;
constexpr uint8_t kInputDown = 1;
constexpr uint8_t kInputPressed = 2;
constexpr uint8_t kInputReleased = 4;
constexpr int32_t kPointerTouchMax = 10;

std::optional<uint32_t> controllerUiKey(ControllerAction action) {
    switch (action) {
    case ControllerAction::Accept:
        return 208;
    case ControllerAction::Back:
        return 210;
    case ControllerAction::Pause:
        return 209;
    case ControllerAction::DpadUp:
        return 204;
    case ControllerAction::DpadDown:
        return 205;
    case ControllerAction::DpadLeft:
        return 206;
    case ControllerAction::DpadRight:
        return 207;
    case ControllerAction::Sprint:
    case ControllerAction::Reload:
    case ControllerAction::ChangeWeapon:
    case ControllerAction::TacticalGrenade:
    case ControllerAction::Grenade:
    case ControllerAction::Aim:
    case ControllerAction::Fire:
    case ControllerAction::Melee:
    case ControllerAction::Count:
        return std::nullopt;
    }
    return std::nullopt;
}

bool guestActionPressed(
    GuestControllerAction action,
    const std::array<bool, static_cast<std::size_t>(ControllerAction::Count)> &states) {
    const auto held = [&states](ControllerAction control) {
        return states[static_cast<std::size_t>(control)];
    };
    switch (action) {
    case GuestControllerAction::Sprint:
        return held(ControllerAction::Sprint);
    case GuestControllerAction::Reload:
        return held(ControllerAction::Reload);
    case GuestControllerAction::Melee:
        return held(ControllerAction::Melee);
    case GuestControllerAction::Grenade:
        return held(ControllerAction::Grenade);
    case GuestControllerAction::Aim:
        return held(ControllerAction::Aim);
    case GuestControllerAction::Fire:
        return held(ControllerAction::Fire);
    case GuestControllerAction::TacticalGrenade:
        return held(ControllerAction::TacticalGrenade);
    case GuestControllerAction::CrouchProne:
        return held(ControllerAction::Back);
    case GuestControllerAction::AlternateFire:
        return false;
    case GuestControllerAction::ChangeWeapon:
        return held(ControllerAction::ChangeWeapon);
    case GuestControllerAction::Count:
        return false;
    }
    return false;
}
} // namespace

Runtime::Runtime(GuestMemory &mem, std::vector<std::string> symbols, Config config,
                 std::unique_ptr<AudioBackend> audioBackend,
                 std::unique_ptr<ZeroConfBackend> zeroConfBackend)
    : mem_(mem), symbols_(std::move(symbols)), config_(config),
      controlConfig_(ControlConfig::load(
          config.controlConfigPath.empty()
              ? (std::filesystem::path(config.root).parent_path() / "config" / "config.txt")
                    .string()
              : config.controlConfigPath)),
      fs_(config.root.empty() ? std::string(".") : config.root,
          config.saveRoot.empty()
              ? (std::filesystem::path(config.root.empty() ? "." : config.root).parent_path() /
                 "saves")
                    .string()
              : config.saveRoot),
      heap_(mem, config.heapBase, config.heapSize), audioBackend_(std::move(audioBackend)),
      zeroConfBackend_(std::move(zeroConfBackend)), start_(std::chrono::steady_clock::now()),
      xperiaControls_(config.xperiaControls) {
    if (!config_.display.isValid()) {
        throw std::invalid_argument("invalid guest display size");
    }
    if (audioBackend_) {
        const int32_t outputRate = audioBackend_->outputSampleRate();
        if (outputRate > 0 && outputRate <= 0x40000) {
            soundOutputRate_ = outputRate;
        }
        voiceChatEnabled_ = controlConfig_.voiceChatEnabled() && audioBackend_->voiceAvailable();
    }
    pointerX_ = static_cast<int32_t>(config_.display.width * 110u / 640u);
    pointerY_ = static_cast<int32_t>(config_.display.height * 150u / 480u);
    pointerPublishedX_ = pointerX_;
    pointerPublishedY_ = pointerY_;
    mem_.mapRegion(config_.stringArena, config_.stringArenaSize);
    stringCursor_ = config_.stringArena;
    loggedImports_.resize(symbols_.size());
    registerHandlers();
    registerExtensions();
    bindImportHandlers();
}

Runtime::~Runtime() {
    {
        std::lock_guard lock(lifecycleMutex_);
#ifdef __ANDROID__
        if (pendingSurface_) {
            ANativeWindow_release(static_cast<ANativeWindow *>(pendingSurface_));
        }
#endif
        pendingSurface_ = nullptr;
    }
    stopAudioUnit();
    if (zeroConfBackend_) {
        zeroConfBackend_->shutdown();
    }
    if (audioBackend_) {
        audioBackend_->shutdown();
    }
}

void Runtime::requestStop() {
    stopRequested_.store(true, std::memory_order_release);
    lifecycleCondition_.notify_all();
    interruptGuest();
}

void Runtime::setPaused(bool paused) {
    std::lock_guard lock(lifecycleMutex_);
    pauseRequested_ = paused;
    if (paused) {
        detachRequested_ = true;
    }
    lifecycleCondition_.notify_all();
    interruptGuest();
}

bool Runtime::attachSurface(void *nativeWindow) {
    if (!nativeWindow)
        return false;
#ifdef __ANDROID__
    ANativeWindow_acquire(static_cast<ANativeWindow *>(nativeWindow));
#endif
    std::lock_guard lock(lifecycleMutex_);
#ifdef __ANDROID__
    if (pendingSurface_) {
        ANativeWindow_release(static_cast<ANativeWindow *>(pendingSurface_));
    }
#endif
    pendingSurface_ = nativeWindow;
    lifecycleCondition_.notify_all();
    interruptGuest();
    return true;
}

void Runtime::interruptGuest() {
    std::lock_guard lock(cpuMutex_);
    if (cpu_) {
        cpu_->halt();
    }
}

void Runtime::serviceLifecycle() {
    std::unique_lock lock(lifecycleMutex_);
    for (;;) {
        if (stopRequested_.load(std::memory_order_acquire)) {
#ifdef __ANDROID__
            if (pendingSurface_) {
                ANativeWindow_release(static_cast<ANativeWindow *>(pendingSurface_));
            }
#endif
            pendingSurface_ = nullptr;
            return;
        }
        if (detachRequested_) {
            detachRequested_ = false;
            lock.unlock();
            glBridge_.suspendSurface();
            lock.lock();
            continue;
        }
        if (pendingSurface_) {
            void *surface = pendingSurface_;
            pendingSurface_ = nullptr;
            lock.unlock();
            glBridge_.attachSurface(surface);
#ifdef __ANDROID__
            ANativeWindow_release(static_cast<ANativeWindow *>(surface));
#endif
            lock.lock();
            continue;
        }
        if (!pauseRequested_) {
            return;
        }
        lifecycleCondition_.wait(lock);
    }
}

void Runtime::queuePointerEvent(int32_t action, int32_t touchId, int32_t x, int32_t y) {
    HostPointerEvent event{
        action,
        touchId,
        std::clamp(x, 0, static_cast<int32_t>(config_.display.width - 1)),
        std::clamp(y, 0, static_cast<int32_t>(config_.display.height - 1)),
    };
    std::lock_guard lock(pointerMutex_);
    if (pointerEvents_.size() >= 256) {
        const auto staleMove =
            std::find_if(pointerEvents_.begin(), pointerEvents_.end(),
                         [](const HostPointerEvent &pending) { return pending.action == 2; });
        if (staleMove != pointerEvents_.end()) {
            pointerEvents_.erase(staleMove);
        } else if (action == 2) {
            return;
        }
    }
    pointerEvents_.push_back(event);
}

void Runtime::queueControllerButton(ControllerAction action, bool pressed) {
    if (action < ControllerAction::Accept || action >= ControllerAction::Count) {
        return;
    }
    std::lock_guard lock(controllerMutex_);
    if (!controllerButtons_.empty() && controllerButtons_.back().action == action &&
        controllerButtons_.back().pressed == pressed) {
        return;
    }
    if (controllerButtons_.size() >= 128) {
        controllerButtons_.clear();
        controllerReleasePending_ = true;
    }
    controllerButtons_.push_back({action, pressed});
}

bool Runtime::queueGuestControllerAction(GuestControllerAction action, bool pressed) {
    const auto call = controllerBridge_.resolve(mem_, action, pressed);
    if (!call) {
        return false;
    }
    PendingGuestCallback pending;
    pending.function = call->function;
    pending.arguments = call->arguments;
    pending.customArguments = true;
    pending.valid = [this, call = *call] { return controllerBridge_.valid(mem_, call); };
    pendingGuestCallbacks_.push_back(std::move(pending));
    return true;
}

void Runtime::queueControllerAxes(float leftX, float leftY, float rightX, float rightY) {
    std::lock_guard lock(controllerMutex_);
    pendingControllerAxes_ = {std::clamp(leftX, -1.0f, 1.0f), std::clamp(leftY, -1.0f, 1.0f),
                              std::clamp(rightX, -1.0f, 1.0f), std::clamp(rightY, -1.0f, 1.0f)};
    controllerAxesDirty_ = true;
}

void Runtime::releaseController() {
    std::lock_guard lock(controllerMutex_);
    controllerButtons_.clear();
    pendingControllerAxes_ = {};
    controllerAxesDirty_ = true;
    controllerReleasePending_ = true;
}

void Runtime::setControllerConnected(bool connected) {
    xperiaControls_.store(connected, std::memory_order_release);
}

void Runtime::queueZeroConfFound(uint32_t searchHandle, std::string serviceKey, std::string name,
                                 std::string serviceType, std::string domain, std::string host,
                                 uint16_t port, uint32_t ipv4Address,
                                 std::vector<std::string> txtRecords) {
    std::lock_guard lock(zeroConfMutex_);
    zeroConfEvents_.push_back({ZeroConfEventType::Found, searchHandle, std::move(serviceKey),
                               std::move(name), std::move(serviceType), std::move(domain),
                               std::move(host), port, ipv4Address, std::move(txtRecords)});
}

void Runtime::queueZeroConfLost(uint32_t searchHandle, std::string serviceKey) {
    std::lock_guard lock(zeroConfMutex_);
    ZeroConfEvent event;
    event.type = ZeroConfEventType::Lost;
    event.handle = searchHandle;
    event.serviceKey = std::move(serviceKey);
    zeroConfEvents_.push_back(std::move(event));
}

void Runtime::queueZeroConfFailure(uint32_t handle, bool publication) {
    std::lock_guard lock(zeroConfMutex_);
    ZeroConfEvent event;
    event.type =
        publication ? ZeroConfEventType::PublicationFailure : ZeroConfEventType::SearchFailure;
    event.handle = handle;
    zeroConfEvents_.push_back(std::move(event));
}

void Runtime::clearPointerTransitions() {
    pointerState_ &= static_cast<uint8_t>(~(kInputPressed | kInputReleased));
    for (auto &state : touchpadState_) {
        state &= static_cast<uint8_t>(~(kInputPressed | kInputReleased));
    }
    for (auto &[id, touch] : touchPoints_) {
        (void)id;
        touch.state &= static_cast<uint8_t>(~(kInputPressed | kInputReleased));
    }
}

void Runtime::pumpControllerInput() {
    std::deque<ControllerButtonEvent> buttons;
    std::array<float, 4> axes{};
    bool axesDirty = false;
    bool release = false;
    {
        std::lock_guard lock(controllerMutex_);
        buttons.swap(controllerButtons_);
        axes = pendingControllerAxes_;
        axesDirty = controllerAxesDirty_;
        release = controllerReleasePending_;
        controllerAxesDirty_ = false;
        controllerReleasePending_ = false;
    }

    auto queueCallback = [this](const PointerCallback &callback, const auto &event) {
        if (!callback.function) {
            return;
        }
        PendingGuestCallback pending;
        pending.function = callback.function;
        pending.userData = callback.userData;
        pending.payload.resize(sizeof(event));
        std::memcpy(pending.payload.data(), &event, sizeof(event));
        pendingGuestCallbacks_.push_back(std::move(pending));
    };
    auto setUiKey = [this](uint32_t key, bool down) {
        const bool wasDown = (controllerUiLive_[key] & kInputDown) != 0;
        if (wasDown == down) {
            return;
        }
        if (down) {
            controllerUiLive_[key] |= kInputDown | kInputPressed;
        } else {
            controllerUiLive_[key] &= static_cast<uint8_t>(~kInputDown);
            controllerUiLive_[key] |= kInputReleased;
        }
    };
    auto syncGuestActions = [this] {
        for (std::size_t index = 0; index < guestControllerActions_.size(); ++index) {
            const auto action = static_cast<GuestControllerAction>(index);
            const bool pressed =
                !controllerUiActive_ && guestActionPressed(action, controllerActions_);
            if (guestControllerActions_[index] != pressed &&
                queueGuestControllerAction(action, pressed)) {
                guestControllerActions_[index] = pressed;
            }
        }
    };
    auto applyButton = [this, &setUiKey, &syncGuestActions](ControllerAction action, bool pressed) {
        const auto index = static_cast<std::size_t>(action);
        if (controllerActions_[index] == pressed) {
            return;
        }
        controllerActions_[index] = pressed;
        if (const auto key = controllerUiKey(action)) {
            setUiKey(*key, pressed);
        }
        syncGuestActions();
    };
    auto releaseTouchpad = [this, &queueCallback](std::size_t id) {
        if (!touchpadActive_[id]) {
            return;
        }
        touchpadActive_[id] = false;
        touchpadState_[id] = kInputReleased;
        queueCallback(touchpadCallbacks_[0],
                      guest::TouchpadButtonEvent{static_cast<int32_t>(id), 0, touchpadX_[id],
                                                 touchpadY_[id]});
    };

    if (release) {
        for (std::size_t i = 0; i < controllerActions_.size(); ++i) {
            applyButton(static_cast<ControllerAction>(i), false);
        }
    }
    for (const auto &button : buttons) {
        logf("[input] controller action=%d pressed=%d\n", static_cast<int32_t>(button.action),
             button.pressed ? 1 : 0);
        applyButton(button.action, button.pressed);
    }
    for (std::size_t index = 0; index < controllerActions_.size(); ++index) {
        if (const auto key = controllerUiKey(static_cast<ControllerAction>(index))) {
            setUiKey(*key, controllerActions_[index]);
        }
    }
    syncGuestActions();

    if (axesDirty) {
        controllerAxes_ = axes;
    }

    if (!axesDirty) {
        return;
    }
    constexpr std::array<int32_t, 2> centerX = {kXperiaTouchpadWidth / 5,
                                                kXperiaTouchpadWidth * 4 / 5};
    constexpr std::array<int32_t, 2> radiusX = {kXperiaTouchpadWidth / 5, kXperiaTouchpadWidth / 8};
    constexpr std::array<int32_t, 2> radiusY = {kXperiaTouchpadHeight / 2,
                                                kXperiaTouchpadWidth / 8};
    for (std::size_t id = 0; id < 2; ++id) {
        float xAxis = axes[id * 2];
        float yAxis = axes[id * 2 + 1];
        if (std::abs(xAxis) <= kXperiaAxisDeadzone)
            xAxis = 0;
        if (std::abs(yAxis) <= kXperiaAxisDeadzone)
            yAxis = 0;
        if (xAxis == 0 && yAxis == 0) {
            releaseTouchpad(id);
            continue;
        }
        const int32_t x = std::clamp(centerX[id] + static_cast<int32_t>(xAxis * radiusX[id]), 0,
                                     kXperiaTouchpadWidth - 1);
        const int32_t y =
            std::clamp(kXperiaTouchpadHeight / 2 + static_cast<int32_t>(yAxis * radiusY[id]), 0,
                       kXperiaTouchpadHeight - 1);
        if (!touchpadActive_[id]) {
            touchpadActive_[id] = true;
            touchpadState_[id] = kInputDown | kInputPressed;
            touchpadX_[id] = x;
            touchpadY_[id] = y;
            queueCallback(touchpadCallbacks_[1],
                          guest::TouchpadMotionEvent{static_cast<int32_t>(id), x, y});
            queueCallback(touchpadCallbacks_[0],
                          guest::TouchpadButtonEvent{static_cast<int32_t>(id), 1, x, y});
        } else if (touchpadX_[id] != x || touchpadY_[id] != y) {
            touchpadX_[id] = x;
            touchpadY_[id] = y;
            queueCallback(touchpadCallbacks_[1],
                          guest::TouchpadMotionEvent{static_cast<int32_t>(id), x, y});
        }
    }
}

void Runtime::publishKeyboardState() {
    controllerUiActive_ = controllerBridge_.frontendActive(mem_, frontendInputManager_);
    pumpControllerInput();
    keyboardPublished_ = keyboardLive_;
    controllerUiPublished_ = controllerUiLive_;
    for (auto &state : keyboardLive_) {
        state &= static_cast<uint8_t>(~(kInputPressed | kInputReleased));
    }
    for (auto &state : controllerUiLive_) {
        state &= static_cast<uint8_t>(~(kInputPressed | kInputReleased));
    }
}

void Runtime::clearKeyboardState() {
    keyboardLive_.fill(0);
    keyboardPublished_.fill(0);
    controllerUiLive_.fill(0);
    controllerUiPublished_.fill(0);
    frontendInputManager_ = 0;
    controllerUiActive_ = false;
}

void Runtime::publishPointerState() {
    pointerPublishedX_ = pointerX_;
    pointerPublishedY_ = pointerY_;
    pointerPublishedState_ = pointerState_;
    pointerPublishedDown_ = pointerDown_;
    publishedTouchPoints_ = touchPoints_;
}

void Runtime::pumpPointerEvents() {
    pumpControllerInput();
    if (pointerTransitionPending_) {
        return;
    }
    auto queueCallback = [this](std::size_t id, const auto &event) {
        for (const auto &callback : pointerCallbacks_[id].entries()) {
            PendingGuestCallback pending;
            pending.function = callback.function;
            pending.userData = callback.userData;
            pending.payload.resize(sizeof(event));
            std::memcpy(pending.payload.data(), &event, sizeof(event));
            pendingGuestCallbacks_.push_back(std::move(pending));
        }
    };

    bool processedMotion = false;
    for (;;) {
        std::optional<HostPointerEvent> next;
        {
            std::lock_guard lock(pointerMutex_);
            if (!pointerEvents_.empty() &&
                (!processedMotion || pointerEvents_.front().action == 2)) {
                next = pointerEvents_.front();
                pointerEvents_.pop_front();
            }
        }
        if (!next) {
            break;
        }
        auto event = *next;
        const auto existingSlot = pointerTouchSlots_.find(event.touchId);
        if (event.action == 0) {
            if (existingSlot != pointerTouchSlots_.end()) {
                continue;
            } else {
                std::array<bool, kPointerTouchMax> used{};
                for (const auto &[hostId, slot] : pointerTouchSlots_) {
                    (void)hostId;
                    if (slot >= 0 && slot < kPointerTouchMax) {
                        used[static_cast<std::size_t>(slot)] = true;
                    }
                }
                const auto available = std::find(used.begin(), used.end(), false);
                if (available == used.end()) {
                    logf("[pointer] ignored action=%d host-id=%d: no touch slot\n", event.action,
                         event.touchId);
                    continue;
                }
                const int32_t slot = static_cast<int32_t>(available - used.begin());
                pointerTouchSlots_.emplace(event.touchId, slot);
                event.touchId = slot;
            }
        } else {
            if (existingSlot == pointerTouchSlots_.end()) {
                logf("[pointer] ignored action=%d unknown host-id=%d\n", event.action,
                     event.touchId);
                continue;
            }
            event.touchId = existingSlot->second;
        }
        auto &touch = touchPoints_[event.touchId];
        const bool touchMoved = event.x != touch.x || event.y != touch.y;
        touch.x = event.x;
        touch.y = event.y;
        if (event.touchId == 0) {
            pointerX_ = event.x;
            pointerY_ = event.y;
        }
        if (event.action == 0) {
            touch.state &= static_cast<uint8_t>(~kInputReleased);
            touch.state |= kInputDown | kInputPressed;
            touch.down = true;
            bool primaryPressed = false;
            if (event.touchId == 0) {
                const bool wasDown = pointerDown_;
                pointerDown_ = true;
                if (!wasDown) {
                    pointerState_ &= static_cast<uint8_t>(~kInputReleased);
                    pointerState_ |= kInputDown | kInputPressed;
                    primaryPressed = true;
                }
            }
            queueCallback(2, guest::PointerTouchEvent{event.touchId, 1, touch.x, touch.y});
            if (primaryPressed) {
                queueCallback(0, guest::PointerButtonEvent{0, 1, pointerX_, pointerY_});
            }
        } else if (event.action == 2) {
            if (!touchMoved) {
                continue;
            }
            queueCallback(3, guest::PointerTouchMotionEvent{event.touchId, touch.x, touch.y});
            if (event.touchId == 0) {
                queueCallback(1, guest::PointerMotionEvent{pointerX_, pointerY_});
            }
        } else if (event.action == 1 || event.action == 3) {
            if (touch.down) {
                touch.state &= static_cast<uint8_t>(~kInputDown);
                touch.state |= kInputReleased;
            }
            touch.down = false;
            bool primaryReleased = false;
            if (event.touchId == 0) {
                if (pointerDown_) {
                    pointerState_ &= static_cast<uint8_t>(~kInputDown);
                    pointerState_ |= kInputReleased;
                    primaryReleased = true;
                }
                pointerDown_ = false;
            }
            queueCallback(2, guest::PointerTouchEvent{event.touchId, 0, touch.x, touch.y});
            if (primaryReleased) {
                queueCallback(0, guest::PointerButtonEvent{0, 0, pointerX_, pointerY_});
            }
            pointerTouchSlots_.erase(next->touchId);
        }
        if (event.action != 2) {
            pointerTransitionPending_ = true;
            break;
        }
        processedMotion = true;
    }
}

void Runtime::queueDueTimers() {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t sequenceLimit = timerSequence_;
    for (;;) {
        auto due = std::min_element(timers_.begin(), timers_.end(),
                                    [](const auto &a, const auto &b) { return a.due < b.due; });
        if (due == timers_.end() || due->due > now || due->sequence > sequenceLimit) {
            break;
        }
        PendingGuestCallback pending;
        pending.function = due->function;
        pending.userData = due->userData;
        pendingGuestCallbacks_.push_back(std::move(pending));
        timers_.erase(due);
    }
}

void Runtime::queueSocketEvents() {
    if (auto result = sockets_.pollLookup()) {
        if (pendingLookup_) {
            const auto lookup = *pendingLookup_;
            pendingLookup_.reset();
            if (result->result == 0) {
                mem_.write(lookup.address, reinterpret_cast<const uint8_t *>(&result->address),
                           sizeof(result->address));
            }
            PendingGuestCallback pending;
            pending.function = lookup.callback;
            pending.userData = lookup.userData;
            pending.customArguments = true;
            pending.arguments = {result->result == 0 ? lookup.address : 0u, lookup.userData, 0, 0};
            pendingGuestCallbacks_.push_back(std::move(pending));
        }
    }
    for (const auto &event : sockets_.poll()) {
        if (!event.callback.function) {
            continue;
        }
        PendingGuestCallback pending;
        pending.function = event.callback.function;
        pending.userData = event.callback.userData;
        pending.customArguments = true;
        pending.arguments[0] = event.handle;
        pending.arguments[1] = 0;
        pending.arguments[2] = event.callback.userData;
        if (event.type == SocketSystem::EventType::Connect) {
            pending.payload.resize(sizeof(event.result));
            std::memcpy(pending.payload.data(), &event.result, sizeof(event.result));
            pending.arguments[1] = kCallbackScratch;
        }
        pendingGuestCallbacks_.push_back(std::move(pending));
    }
}

void Runtime::queueZeroConfEvents() {
    std::deque<ZeroConfEvent> events;
    {
        std::lock_guard lock(zeroConfMutex_);
        events.swap(zeroConfEvents_);
    }
    auto appendString = [](std::vector<uint8_t> &payload, std::string_view value) {
        const GuestAddr address = kCallbackScratch + static_cast<GuestAddr>(payload.size());
        payload.insert(payload.end(), value.begin(), value.end());
        payload.push_back(0);
        return address;
    };
    auto alignPayload = [](std::vector<uint8_t> &payload) {
        while ((payload.size() & 3u) != 0) {
            payload.push_back(0);
        }
    };
    auto queueLost = [this](GuestHandle searchHandle, const ZeroConfSearch &search,
                            GuestAddr serviceToken) {
        if (!search.lostCallback) {
            heap_.free(serviceToken);
            return;
        }
        PendingGuestCallback pending;
        pending.function = search.lostCallback;
        pending.userData = search.userData;
        pending.customArguments = true;
        pending.arguments = {searchHandle, serviceToken, search.userData, 0};
        pending.completion = [this, serviceToken](uint32_t) { heap_.free(serviceToken); };
        pendingGuestCallbacks_.push_back(std::move(pending));
    };
    for (auto &event : events) {
        if (event.type == ZeroConfEventType::SearchFailure) {
            if (auto *search = zeroConfSearches_.get(event.handle)) {
                for (const auto &[key, service] : search->services) {
                    (void)key;
                    if (service.token) {
                        heap_.free(service.token);
                    }
                }
            }
            zeroConfSearches_.release(event.handle);
            continue;
        }
        if (event.type == ZeroConfEventType::PublicationFailure) {
            zeroConfPublications_.release(event.handle);
            continue;
        }
        auto *search = zeroConfSearches_.get(event.handle);
        if (!search) {
            continue;
        }
        auto existing = search->services.find(event.serviceKey);
        if (event.type == ZeroConfEventType::Lost) {
            if (existing != search->services.end()) {
                queueLost(event.handle, *search, existing->second.token);
                search->services.erase(existing);
            }
            continue;
        }

        const bool endpointChanged =
            existing != search->services.end() &&
            (existing->second.name != event.name ||
             existing->second.serviceType != event.serviceType ||
             existing->second.domain != event.domain || existing->second.host != event.host ||
             existing->second.port != event.port ||
             existing->second.ipv4Address != event.ipv4Address);
        if (endpointChanged) {
            queueLost(event.handle, *search, existing->second.token);
            search->services.erase(existing);
            existing = search->services.end();
        }
        if (existing != search->services.end()) {
            if (existing->second.txtRecords == event.txtRecords || !search->updateCallback) {
                continue;
            }
            existing->second.txtRecords = event.txtRecords;
            PendingGuestCallback pending;
            pending.function = search->updateCallback;
            pending.userData = search->userData;
            pending.customArguments = true;
            pending.payload.resize(sizeof(guest::ZeroConfTxtUpdate));
            alignPayload(pending.payload);
            std::vector<GuestAddr> txtPointers;
            for (const auto &record : event.txtRecords) {
                txtPointers.push_back(appendString(pending.payload, record));
            }
            alignPayload(pending.payload);
            const GuestAddr pointers =
                kCallbackScratch + static_cast<GuestAddr>(pending.payload.size());
            for (GuestAddr pointer : txtPointers) {
                const auto *bytes = reinterpret_cast<const uint8_t *>(&pointer);
                pending.payload.insert(pending.payload.end(), bytes, bytes + sizeof(pointer));
            }
            guest::ZeroConfTxtUpdate update{};
            update.service_id = existing->second.token;
            update.txt_count = static_cast<uint16_t>(txtPointers.size());
            update.txt_records = txtPointers.empty() ? 0 : pointers;
            std::memcpy(pending.payload.data(), &update, sizeof(update));
            pending.arguments = {event.handle, kCallbackScratch, search->userData, 0};
            pendingGuestCallbacks_.push_back(std::move(pending));
            continue;
        }

        const GuestAddr token = heap_.alloc(sizeof(uint32_t));
        if (!token) {
            continue;
        }
        mem_.write32(token, token);
        ZeroConfDiscovered discovered{token,      event.name, event.serviceType, event.domain,
                                      event.host, event.port, event.ipv4Address, event.txtRecords};
        search->services.emplace(event.serviceKey, discovered);
        if (!search->foundCallback) {
            continue;
        }
        PendingGuestCallback pending;
        pending.function = search->foundCallback;
        pending.userData = search->userData;
        pending.customArguments = true;
        pending.payload.resize(sizeof(guest::ZeroConfFound));
        alignPayload(pending.payload);
        guest::ZeroConfFound found{};
        found.service_id = token;
        found.name = appendString(pending.payload, event.name);
        found.service_type = appendString(pending.payload, event.serviceType);
        found.domain = appendString(pending.payload, event.domain);
        found.host = appendString(pending.payload, event.host);
        std::vector<GuestAddr> txtPointers;
        for (const auto &record : event.txtRecords) {
            txtPointers.push_back(appendString(pending.payload, record));
        }
        alignPayload(pending.payload);
        const GuestAddr pointers =
            kCallbackScratch + static_cast<GuestAddr>(pending.payload.size());
        for (GuestAddr pointer : txtPointers) {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&pointer);
            pending.payload.insert(pending.payload.end(), bytes, bytes + sizeof(pointer));
        }
        found.port = SocketSystem::hostToNetwork16(event.port);
        found.txt_count = static_cast<uint16_t>(txtPointers.size());
        found.txt_records = txtPointers.empty() ? 0 : pointers;
        found.ipv4_address = event.ipv4Address;
        std::memcpy(pending.payload.data(), &found, sizeof(found));
        pending.arguments = {event.handle, kCallbackScratch, search->userData, 0};
        pendingGuestCallbacks_.push_back(std::move(pending));
    }
}

RunOutcome Runtime::runGuestCallbacks() {
    while (!pendingGuestCallbacks_.empty()) {
        auto pending = std::move(pendingGuestCallbacks_.front());
        pendingGuestCallbacks_.pop_front();
        if (!pending.function || pending.payload.size() > kCallbackScratchSize ||
            (pending.valid && !pending.valid())) {
            continue;
        }

        if (!pending.payload.empty()) {
            mem_.write(kCallbackScratch, pending.payload.data(),
                       static_cast<GuestSize>(pending.payload.size()));
        }
        std::array<GuestArgument, 4> arguments{};
        if (pending.customArguments) {
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                arguments[index] = GuestArgument::u32(pending.arguments[index]);
            }
        } else {
            arguments[0] = GuestArgument::u32(pending.payload.empty() ? 0 : kCallbackScratch);
            arguments[1] = GuestArgument::u32(pending.userData);
        }
        runningGuestCallback_ = true;
        GuestCallOptions options;
        options.interrupted = [this] { return stopRequested_.load(std::memory_order_acquire); };
        const auto result = callGuest(*cpu_, mem_, pending.function, arguments, options);
        runningGuestCallback_ = false;
        if (result.status == GuestCallStatus::Interrupted) {
            return RunOutcome::Halted;
        }
        if (result.status == GuestCallStatus::Fault ||
            result.status == GuestCallStatus::RecursionLimit ||
            result.status == GuestCallStatus::InvalidStack ||
            result.status == GuestCallStatus::SliceLimit) {
            logf("[callback] failed status=%d pc=0x%08x detail='%s'\n",
                 static_cast<int>(result.status), result.finalPc, result.detail.c_str());
            return RunOutcome::Fault;
        }
        const uint32_t returnValue = static_cast<uint32_t>(result.returnValue);
        if (pending.completion) {
            pending.completion(returnValue);
        }
    }
    return RunOutcome::Halted;
}

bool Runtime::beginSoundDecode(int32_t channel, GuestAddr data, uint32_t samples, uint32_t repeat,
                               uint32_t loopIndex, bool beginAtLoop) {
    if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size()) || !data ||
        samples == 0 || samples > std::numeric_limits<uint32_t>::max() / sizeof(int16_t)) {
        return false;
    }
    auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
    if ((slot.playing || slot.decoding) && audioBackend_) {
        audioBackend_->stopSound(channel);
    }
    const uint64_t generation = ++slot.generation;
    slot.sourceData = data;
    slot.sourceSamples = samples;
    slot.repeat = repeat;
    slot.loopIndex = loopIndex;
    slot.beginAtLoop = beginAtLoop;
    slot.decoded.clear();
    slot.playing = false;
    slot.paused = false;

    if (slot.callbacks[1].function) {
        slot.decoding = true;
        queueSoundGeneratorChunk(channel, generation);
        return true;
    }

    auto bytes = mem_.read(data, samples * static_cast<uint32_t>(sizeof(int16_t)));
    slot.decoded.resize(samples);
    std::memcpy(slot.decoded.data(), bytes.data(), bytes.size());
    if (beginAtLoop) {
        if (loopIndex >= slot.decoded.size()) {
            slot.decoded.clear();
            return false;
        }
        slot.decoded.erase(slot.decoded.begin(), slot.decoded.begin() + loopIndex);
    }
    const int32_t volume = slot.volume * soundVolume_ / 256;
    slot.playing =
        audioBackend_ && audioBackend_->playSound(channel, slot.decoded, slot.rate, volume);
    logf("[audio] pcm channel=%d samples=%zu rate=%d volume=%d result=%d\n", channel,
         slot.decoded.size(), slot.rate, volume, slot.playing ? 0 : 1);
    return slot.playing;
}

void Runtime::queueSoundGeneratorChunk(int32_t channel, uint64_t generation) {
    auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
    if (!slot.decoding || slot.generation != generation) {
        return;
    }
    const uint64_t limit =
        std::min<uint64_t>(static_cast<uint64_t>(slot.sourceSamples) * kMaxSoundExpansion +
                               kSoundGeneratorChunkSamples,
                           kMaxDecodedSoundSamples);
    if (slot.decoded.size() >= limit) {
        logf("[audio] decode limit channel=%d source=%u decoded=%zu\n", channel, slot.sourceSamples,
             slot.decoded.size());
        slot.decoding = false;
        slot.decoded.clear();
        return;
    }
    const uint32_t requested = static_cast<uint32_t>(
        std::min<uint64_t>(kSoundGeneratorChunkSamples, limit - slot.decoded.size()));
    guest::SoundGenAudioInfo info{channel,         kSoundPcmScratch,   requested, 0,
                                  slot.sourceData, slot.sourceSamples, 0,         0};
    PendingGuestCallback pending;
    pending.function = slot.callbacks[1].function;
    pending.userData = slot.callbacks[1].userData;
    pending.payload.resize(sizeof(info));
    std::memcpy(pending.payload.data(), &info, sizeof(info));
    pending.valid = [this, channel, generation] {
        const auto &active = soundChannels_[static_cast<std::size_t>(channel)];
        return active.decoding && active.generation == generation;
    };
    pending.completion = [this, channel, generation, requested](uint32_t result) {
        auto &active = soundChannels_[static_cast<std::size_t>(channel)];
        if (!active.decoding || active.generation != generation) {
            return;
        }
        const int32_t produced = static_cast<int32_t>(result);
        const bool endSample = mem_.read32(kCallbackScratch + 28u) != 0;
        if (produced < 0 || static_cast<uint32_t>(produced) > requested ||
            (produced == 0 && !endSample)) {
            active.decoding = false;
            active.decoded.clear();
            return;
        }
        if (produced > 0) {
            auto pcm =
                mem_.read(kSoundPcmScratch, static_cast<uint32_t>(produced) * sizeof(int16_t));
            const std::size_t offset = active.decoded.size();
            active.decoded.resize(offset + static_cast<uint32_t>(produced));
            std::memcpy(active.decoded.data() + offset, pcm.data(), pcm.size());
        }
        if (!endSample) {
            queueSoundGeneratorChunk(channel, generation);
            return;
        }
        active.decoding = false;
        if (active.beginAtLoop) {
            if (active.loopIndex >= active.decoded.size()) {
                active.decoded.clear();
                return;
            }
            active.decoded.erase(active.decoded.begin(), active.decoded.begin() + active.loopIndex);
        }
        const int32_t volume = active.volume * soundVolume_ / 256;
        active.playing = audioBackend_ && audioBackend_->playSound(channel, active.decoded,
                                                                   soundOutputRate_, volume);
        logf("[audio] pcm channel=%d samples=%zu rate=%d volume=%d result=%d\n", channel,
             active.decoded.size(), soundOutputRate_, volume, active.playing ? 0 : 1);
    };
    pendingGuestCallbacks_.push_back(std::move(pending));
}

void Runtime::finishSoundChannel(int32_t channel, bool explicitStop) {
    auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
    const bool wasActive = slot.playing || slot.decoding;
    logf("[audio] channelFinish channel=%d explicit=%u repeat=%u callbacks=%u%u%u\n", channel,
         explicitStop ? 1u : 0u, slot.repeat, slot.callbacks[0].function ? 1u : 0u,
         slot.callbacks[1].function ? 1u : 0u, slot.callbacks[2].function ? 1u : 0u);
    if (audioBackend_) {
        audioBackend_->stopSound(channel);
    }
    guest::SoundEndSampleInfo info{channel, static_cast<int32_t>(slot.repeat), 0,
                                   slot.sourceSamples};
    if (!explicitStop && slot.repeat > 0) {
        --info.reps_remaining;
    }
    const GuestAddr sourceData = slot.sourceData;
    const uint32_t sourceSamples = slot.sourceSamples;
    const uint32_t loopIndex = slot.loopIndex;
    slot.playing = false;
    slot.decoding = false;
    ++slot.generation;
    slot.paused = false;
    slot.decoded.clear();

    if (explicitStop) {
        if (wasActive && slot.callbacks[2].function) {
            PendingGuestCallback pending;
            pending.function = slot.callbacks[2].function;
            pending.userData = slot.callbacks[2].userData;
            pending.payload.resize(sizeof(info));
            std::memcpy(pending.payload.data(), &info, sizeof(info));
            pendingGuestCallbacks_.push_back(std::move(pending));
        }
        return;
    }

    auto repeatCompletion = [this, channel, info, sourceData, sourceSamples,
                             loopIndex](uint32_t keepPlaying) {
        guest::SoundEndSampleInfo updated = info;
        const auto bytes = mem_.read(kCallbackScratch, sizeof(updated));
        std::memcpy(&updated, bytes.data(), sizeof(updated));
        if (keepPlaying) {
            const GuestAddr nextData = updated.new_data ? updated.new_data : sourceData;
            const uint32_t nextSamples = updated.new_data ? updated.num_samples : sourceSamples;
            const uint32_t nextRepeat =
                updated.reps_remaining > 0 ? static_cast<uint32_t>(updated.reps_remaining) : 0u;
            if (beginSoundDecode(channel, nextData, nextSamples, nextRepeat, loopIndex,
                                 updated.new_data == 0)) {
                return;
            }
        }
        auto &finished = soundChannels_[static_cast<std::size_t>(channel)];
        if (finished.callbacks[2].function) {
            PendingGuestCallback stopped;
            stopped.function = finished.callbacks[2].function;
            stopped.userData = finished.callbacks[2].userData;
            stopped.payload.resize(sizeof(updated));
            std::memcpy(stopped.payload.data(), &updated, sizeof(updated));
            pendingGuestCallbacks_.push_back(std::move(stopped));
        }
    };

    if (slot.callbacks[0].function) {
        PendingGuestCallback pending;
        pending.function = slot.callbacks[0].function;
        pending.userData = slot.callbacks[0].userData;
        pending.payload.resize(sizeof(info));
        std::memcpy(pending.payload.data(), &info, sizeof(info));
        pending.completion = std::move(repeatCompletion);
        pendingGuestCallbacks_.push_back(std::move(pending));
    } else {
        if (slot.repeat == 0 || info.reps_remaining > 0) {
            const uint32_t nextRepeat =
                info.reps_remaining > 0 ? static_cast<uint32_t>(info.reps_remaining) : 0u;
            if (beginSoundDecode(channel, sourceData, sourceSamples, nextRepeat, loopIndex, true)) {
                return;
            }
        }
        if (slot.callbacks[2].function) {
            PendingGuestCallback stopped;
            stopped.function = slot.callbacks[2].function;
            stopped.userData = slot.callbacks[2].userData;
            stopped.payload.resize(sizeof(info));
            std::memcpy(stopped.payload.data(), &info, sizeof(info));
            pendingGuestCallbacks_.push_back(std::move(stopped));
        }
    }
}

void Runtime::serviceSoundChannels() {
    if (!audioBackend_) {
        return;
    }
    const bool hasActiveChannel = std::ranges::any_of(
        soundChannels_, [](const auto &slot) { return slot.playing && !slot.paused; });
    if (!hasActiveChannel) {
        return;
    }
    const uint32_t active = audioBackend_->activeSoundMask();
    for (int32_t channel = 0; channel < static_cast<int32_t>(soundChannels_.size()); ++channel) {
        const auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
        if (slot.playing && !slot.paused && (active & (1u << channel)) == 0) {
            finishSoundChannel(channel, false);
        }
    }
}

GuestAddr Runtime::addExtension(Handler h) {
    GuestAddr trap = nextExtTrap_;
    nextExtTrap_ += 4;
    extHandlers_[trap] = std::move(h);
    return trap;
}

void Runtime::registerExtensions() {
    auto noop = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    auto readString = [this](GuestAddr address, std::size_t limit) {
        return address ? mem_.readString(address, static_cast<GuestSize>(limit)) : std::string();
    };
    auto readTxtRecords = [this, readString](GuestAddr pointers, uint32_t count,
                                             std::vector<std::string> &records) {
        if (count > 16 || (count && !pointers) ||
            (count && !mem_.isMapped(pointers, count * sizeof(GuestAddr)))) {
            return false;
        }
        records.clear();
        records.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            const GuestAddr address = mem_.read32(pointers + index * sizeof(GuestAddr));
            if (!address) {
                return false;
            }
            const std::string record = readString(address, 256);
            if (record.size() > 255) {
                return false;
            }
            records.push_back(record);
        }
        return true;
    };

    extTables_[kHashTouchpad] = {
        addExtension([this](GuestCpu &cpu) {
            const uint32_t id = cpu.reg(0);
            if (id < touchpadCallbacks_.size()) {
                touchpadCallbacks_[id] = {cpu.reg(1), cpu.reg(2)};
            }
            cpu.setReg(0, 0);
        }),
        addExtension([this](GuestCpu &cpu) {
            const uint32_t id = cpu.reg(0);
            if (id < touchpadCallbacks_.size() &&
                (cpu.reg(1) == 0 || touchpadCallbacks_[id].function == cpu.reg(1))) {
                touchpadCallbacks_[id] = {};
            }
            cpu.setReg(0, 0);
        }),
        addExtension(noop), addExtension(noop), addExtension([](GuestCpu &cpu) {
            uint32_t value = 0;
            switch (cpu.reg(0)) {
            case 0:
                value = 1;
                break;
            case 1:
                value = kXperiaTouchpadWidth;
                break;
            case 2:
                value = kXperiaTouchpadHeight;
                break;
            default:
                break;
            }
            cpu.setReg(0, value);
        })};

    // ZeroConf: start/stop/publish/update/unpublish.
    extTables_[kHashZeroConf] = {
        addExtension([this, readString](GuestCpu &cpu) {
            if (!zeroConfBackend_ || !cpu.reg(0)) {
                logf("[zeroconf] search rejected backend=%u type=0x%08x\n",
                     zeroConfBackend_ ? 1u : 0u, cpu.reg(0));
                cpu.setReg(0, 0);
                return;
            }
            ZeroConfSearch search;
            search.foundCallback = cpu.reg(2);
            search.updateCallback = cpu.reg(3);
            search.lostCallback = cpu.stackArg(0);
            search.userData = cpu.stackArg(1);
            const GuestHandle handle = zeroConfSearches_.create(std::move(search));
            const std::string serviceType = readString(cpu.reg(0), 256);
            const std::string domain = cpu.reg(1) ? readString(cpu.reg(1), 256) : "local";
            if (!zeroConfBackend_->startSearch(handle, serviceType, domain)) {
                logf("[zeroconf] search backend rejected type='%s' domain='%s'\n",
                     serviceType.c_str(), domain.c_str());
                zeroConfSearches_.release(handle);
                cpu.setReg(0, 0);
                return;
            }
            logf("[zeroconf] search start handle=0x%08x type='%s' domain='%s'\n", handle,
                 serviceType.c_str(), domain.c_str());
            cpu.setReg(0, handle);
        }),
        addExtension([this](GuestCpu &cpu) {
            const GuestHandle handle = cpu.reg(0);
            if (auto *search = zeroConfSearches_.get(handle)) {
                if (zeroConfBackend_) {
                    zeroConfBackend_->stopSearch(handle);
                }
                for (const auto &[key, service] : search->services) {
                    (void)key;
                    if (service.token) {
                        heap_.free(service.token);
                    }
                }
                zeroConfSearches_.release(handle);
                logf("[zeroconf] search stop handle=0x%08x\n", handle);
            }
            cpu.setReg(0, 0);
        }),
        addExtension([this, readString, readTxtRecords](GuestCpu &cpu) {
            std::vector<std::string> records;
            const uint32_t count = cpu.stackArg(0);
            if (!zeroConfBackend_ || !cpu.reg(0) || !cpu.reg(1) || !cpu.reg(2) ||
                !readTxtRecords(cpu.stackArg(1), count, records)) {
                logf("[zeroconf] publish rejected backend=%u port=0x%04x name=0x%08x "
                     "type=0x%08x txt-count=%u txt=0x%08x\n",
                     zeroConfBackend_ ? 1u : 0u, cpu.reg(0) & 0xffffu, cpu.reg(1), cpu.reg(2),
                     count, cpu.stackArg(1));
                cpu.setReg(0, 0);
                return;
            }
            const GuestHandle handle = zeroConfPublications_.create({});
            const uint16_t port = SocketSystem::networkToHost16(static_cast<uint16_t>(cpu.reg(0)));
            const std::string name = readString(cpu.reg(1), 65);
            const std::string serviceType = readString(cpu.reg(2), 256);
            const std::string domain = cpu.reg(3) ? readString(cpu.reg(3), 256) : "local";
            if (!zeroConfBackend_->publish(handle, port, name, serviceType, domain, records)) {
                logf("[zeroconf] publish backend rejected name='%s' type='%s' domain='%s' "
                     "port=%u txt-count=%zu\n",
                     name.c_str(), serviceType.c_str(), domain.c_str(), port, records.size());
                zeroConfPublications_.release(handle);
                cpu.setReg(0, 0);
                return;
            }
            logf("[zeroconf] publish handle=0x%08x type='%s' port=%u\n", handle,
                 serviceType.c_str(), port);
            cpu.setReg(0, handle);
        }),
        addExtension([this, readTxtRecords](GuestCpu &cpu) {
            std::vector<std::string> records;
            const GuestHandle handle = cpu.reg(0);
            const bool valid = zeroConfPublications_.get(handle) &&
                               readTxtRecords(cpu.reg(2), cpu.reg(1), records);
            const bool updated =
                valid && zeroConfBackend_ && zeroConfBackend_->update(handle, records);
            if (!updated) {
                logf("[zeroconf] update rejected handle=0x%08x valid=%u backend=%u txt-count=%u\n",
                     handle, valid ? 1u : 0u, zeroConfBackend_ ? 1u : 0u, cpu.reg(1));
            }
            cpu.setReg(0, updated ? 0 : 1);
        }),
        addExtension([this](GuestCpu &cpu) {
            const GuestHandle handle = cpu.reg(0);
            if (!zeroConfPublications_.get(handle)) {
                cpu.setReg(0, 1);
                return;
            }
            if (zeroConfBackend_) {
                zeroConfBackend_->unpublish(handle);
            }
            zeroConfPublications_.release(handle);
            logf("[zeroconf] unpublish handle=0x%08x\n", handle);
            cpu.setReg(0, 0);
        })};

    // IS device: register/unregister/setTabletThreshold/getDisplayType/getExternalResources.
    extTables_[kHashIsDevice] = {
        addExtension(noop), addExtension(noop),
        addExtension([](GuestCpu &cpu) { cpu.setReg(0, cpu.reg(0)); }), // threshold passthrough
        addExtension([](GuestCpu &cpu) { cpu.setReg(0, 2); }),          // display type
        addExtension([this](GuestCpu &cpu) { cpu.setReg(0, internString("")); })};

    // Audio unit: 13 entries (7 unit methods + 6 s3eAudio*).
    std::vector<GuestAddr> audio;
    audio.push_back(addExtension(noop));
    audio.push_back(addExtension(noop));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        cpu.setReg(0, voiceChatEnabled_ ? 0u : std::numeric_limits<uint32_t>::max());
    }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        stopAudioUnit();
        audioUnitCaptureCallback_ = 0;
        audioUnitRenderCallback_ = 0;
        cpu.setReg(0, 0);
    }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        audioUnitCaptureCallback_ = cpu.reg(0);
        audioUnitRenderCallback_ = cpu.reg(1);
        if (audioUnitStartRequested_ && (audioUnitCaptureCallback_ || audioUnitRenderCallback_)) {
            activateAudioUnit();
        }
        cpu.setReg(0, voiceChatEnabled_ ? 0u : 1u);
    }));
    audio.push_back(addExtension([this](GuestCpu &cpu) { cpu.setReg(0, startAudioUnit()); }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        stopAudioUnit();
        cpu.setReg(0, 0);
    }));
    audio.push_back(
        addExtension([this](GuestCpu &cpu) { cpu.setReg(0, playMusic(cpu.reg(0), cpu.reg(1))); }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        if (audioBackend_) {
            audioBackend_->stopMusic();
        }
        cpu.setReg(0, 0);
    }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        if (audioBackend_) {
            audioBackend_->resumeMusic();
        }
        cpu.setReg(0, 0);
    }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        if (audioBackend_) {
            audioBackend_->pauseMusic();
        }
        cpu.setReg(0, 0);
    }));
    audio.push_back(addExtension([this](GuestCpu &cpu) {
        cpu.setReg(0, setAudioInt(cpu.reg(0), static_cast<int32_t>(cpu.reg(1))));
    }));
    audio.push_back(addExtension(
        [this](GuestCpu &cpu) { cpu.setReg(0, static_cast<uint32_t>(getAudioInt(cpu.reg(0)))); }));
    extTables_[kHashAudioUnit] = std::move(audio);
}

uint32_t Runtime::playMusic(GuestAddr filename, uint32_t repeat) {
    const std::string path = filename ? mem_.readString(filename, 512) : std::string();
    const auto resolved = fs_.hostPath(path);
    const bool played = resolved && audioBackend_ && audioBackend_->playMusic(*resolved, repeat);
    logf("[audio] play path='%s' repeat=%u result=%d\n", path.c_str(), repeat, played ? 0 : 1);
    return played ? 0u : 1u;
}

uint32_t Runtime::setAudioInt(uint32_t key, int32_t value) {
    switch (key) {
    case 0:
        musicVolume_ = std::clamp(value, 0, 256);
        if (audioBackend_) {
            audioBackend_->setMusicVolume(musicVolume_);
        }
        return 0;
    case 4:
        return value == 0 ? 0u : 1u;
    default:
        return 1;
    }
}

int32_t Runtime::getAudioInt(uint32_t key) const {
    switch (key) {
    case 0:
        return musicVolume_;
    case 1:
        return audioBackend_ ? audioBackend_->musicStatus() : 0;
    case 4:
        return 0;
    case 5:
        return 1;
    case 6:
    case 9:
        return audioBackend_ && audioBackend_->available() ? 1 : 0;
    default:
        return -1;
    }
}

uint32_t Runtime::startAudioUnit() {
    if (audioUnitStartRequested_) {
        return 0;
    }
    ++audioUnitStartAttempts_;
    if (!voiceChatEnabled_ || !audioBackend_) {
        ++audioUnitStartFailures_;
        return 1;
    }
    audioUnitStartRequested_ = true;
    if (!audioUnitCaptureCallback_ && !audioUnitRenderCallback_) {
        return 0;
    }
    return activateAudioUnit() ? 0 : 1;
}

bool Runtime::activateAudioUnit() {
    if (audioUnitStarted_) {
        return true;
    }
    if (!audioUnitStartRequested_ || !audioBackend_ ||
        (!audioUnitCaptureCallback_ && !audioUnitRenderCallback_)) {
        return false;
    }
    if (!audioBackend_->startVoice()) {
        audioUnitStartRequested_ = false;
        ++audioUnitStartFailures_;
        return false;
    }
    audioUnitStarted_ = true;
    audioUnitRenderPending_ = false;
    ++audioUnitGeneration_;
    nextAudioUnitRender_ = std::chrono::steady_clock::now();
    logf("[voice] started capture=0x%08x render=0x%08x\n", audioUnitCaptureCallback_,
         audioUnitRenderCallback_);
    return true;
}

void Runtime::stopAudioUnit() {
    audioUnitStartRequested_ = false;
    if (audioUnitStarted_ && audioBackend_) {
        audioBackend_->stopVoice();
    }
    if (audioUnitStarted_) {
        logf("[voice] stopped\n");
    }
    audioUnitStarted_ = false;
    audioUnitRenderPending_ = false;
    ++audioUnitGeneration_;
}

void Runtime::serviceAudioUnit() {
    if (!audioUnitStarted_ || !audioBackend_) {
        return;
    }
    const uint64_t generation = audioUnitGeneration_;
    for (int chunkIndex = 0; chunkIndex < 2 && audioUnitCaptureCallback_; ++chunkIndex) {
        auto input = audioBackend_->takeVoiceInput();
        if (!input) {
            break;
        }
        if (input->samples.empty() || input->samples.size() > 4096 || input->sampleRate <= 0) {
            continue;
        }
        ++audioUnitCaptureChunks_;
        audioUnitCaptureSamples_ += input->samples.size();
        if (std::any_of(input->samples.begin(), input->samples.end(),
                        [](int16_t sample) { return sample != 0; })) {
            ++audioUnitCaptureNonSilentChunks_;
        }
        PendingGuestCallback pending;
        pending.function = audioUnitCaptureCallback_;
        pending.payload.resize(sizeof(guest::AudioUnitBuffer) +
                               input->samples.size() * sizeof(int16_t));
        const guest::AudioUnitBuffer buffer{
            static_cast<uint32_t>(input->samples.size()),
            kCallbackScratch + static_cast<GuestAddr>(sizeof(guest::AudioUnitBuffer)),
            static_cast<uint32_t>(input->sampleRate),
        };
        std::memcpy(pending.payload.data(), &buffer, sizeof(buffer));
        std::memcpy(pending.payload.data() + sizeof(buffer), input->samples.data(),
                    input->samples.size() * sizeof(int16_t));
        pending.valid = [this, generation] {
            return audioUnitStarted_ && audioUnitGeneration_ == generation;
        };
        pending.completion = [this, generation](uint32_t) {
            if (audioUnitStarted_ && audioUnitGeneration_ == generation) {
                ++audioUnitCaptureCallbacks_;
            }
        };
        pendingGuestCallbacks_.push_back(std::move(pending));
    }

    const auto now = std::chrono::steady_clock::now();
    if (!audioUnitRenderCallback_ || audioUnitRenderPending_ || now < nextAudioUnitRender_) {
        return;
    }
    const uint32_t sampleCount = static_cast<uint32_t>(std::max(soundOutputRate_ / 50, 1));
    PendingGuestCallback pending;
    pending.function = audioUnitRenderCallback_;
    pending.payload.resize(sizeof(guest::AudioUnitBuffer) + sampleCount * sizeof(int16_t));
    const guest::AudioUnitBuffer buffer{
        sampleCount,
        kCallbackScratch + static_cast<GuestAddr>(sizeof(guest::AudioUnitBuffer)),
        static_cast<uint32_t>(soundOutputRate_),
    };
    std::memcpy(pending.payload.data(), &buffer, sizeof(buffer));
    pending.valid = [this, generation] {
        return audioUnitStarted_ && audioUnitGeneration_ == generation;
    };
    pending.completion = [this, generation, sampleCount](uint32_t) {
        audioUnitRenderPending_ = false;
        if (!audioUnitStarted_ || audioUnitGeneration_ != generation || !audioBackend_) {
            return;
        }
        std::vector<int16_t> samples(sampleCount);
        mem_.copyOut(kCallbackScratch + sizeof(guest::AudioUnitBuffer),
                     reinterpret_cast<uint8_t *>(samples.data()),
                     static_cast<GuestSize>(samples.size() * sizeof(int16_t)));
        ++audioUnitRenderCallbacks_;
        if (std::any_of(samples.begin(), samples.end(),
                        [](int16_t sample) { return sample != 0; })) {
            ++audioUnitRenderNonSilentCallbacks_;
        }
        if (audioBackend_->queueVoiceOutput(samples, soundOutputRate_)) {
            ++audioUnitOutputQueued_;
        } else {
            ++audioUnitOutputRejected_;
        }
    };
    audioUnitRenderPending_ = true;
    nextAudioUnitRender_ = now + std::chrono::milliseconds(20);
    pendingGuestCallbacks_.push_back(std::move(pending));
}

uint64_t Runtime::elapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start_)
        .count();
}

GuestAddr Runtime::internString(std::string_view s) {
    auto it = interned_.find(std::string(s));
    if (it != interned_.end()) {
        return it->second;
    }
    GuestAddr addr = stringCursor_;
    std::size_t len = s.size() + 1;
    if (stringCursor_ + len > config_.stringArena + config_.stringArenaSize) {
        return 0;
    }
    mem_.writeString(addr, s);
    stringCursor_ += static_cast<GuestSize>(len);
    interned_[std::string(s)] = addr;
    return addr;
}

void Runtime::recordImport(GuestAddr trapAddr, std::string_view name) {
    if (importCount_ < 64) {
        firstImports_.emplace_back(name);
    }
    lastImportTrap_ = trapAddr;
}

void Runtime::reportProgress(GuestCpu &cpu, std::string_view name) {
    if (!config_.progressCallback) {
        return;
    }
    if (importCount_ < nextProgressCheckImport_) {
        return;
    }
    nextProgressCheckImport_ = importCount_ + 1'024;
    const uint64_t uptime = elapsedMs();
    if (uptime < nextProgressReportMs_) {
        return;
    }
    nextProgressReportMs_ = uptime + 5'000;
    config_.progressCallback({
        .imports = importCount_,
        .pc = cpu.pc(),
        .uptimeMs = uptime,
        .lastImport = std::string(name),
        .renderer = glBridge_.renderer(),
        .glVersion = glBridge_.version(),
        .glDiagnostics = glBridge_.diagnostics(),
        .runtimeDiagnostics = runtimeDiagnostics(),
        .voiceDiagnostics = voiceDiagnostics(),
    });
}

std::string Runtime::runtimeDiagnostics() const {
    const auto averageUs = [](uint64_t totalNs, uint64_t count) {
        return count == 0 ? 0 : totalNs / count / 1'000;
    };
    const uint64_t frames = glBridge_.presentedFrames();
    const double importsPerFrame =
        frames == 0 ? 0.0 : static_cast<double>(importCount_) / static_cast<double>(frames);
    uint64_t recentGuestTotal = 0;
    uint64_t recentHostTotal = 0;
    uint64_t recentGuestMax = 0;
    uint64_t recentHostMax = 0;
    for (std::size_t index = 0; index < recentRuntimeTimeCount_; ++index) {
        recentGuestTotal += recentGuestRunTimesNs_[index];
        recentHostTotal += recentHostServiceTimesNs_[index];
        recentGuestMax = std::max(recentGuestMax, recentGuestRunTimesNs_[index]);
        recentHostMax = std::max(recentHostMax, recentHostServiceTimesNs_[index]);
    }
    const uint64_t averageYieldArgument = yieldCalls_ == 0 ? 0 : yieldArgumentSum_ / yieldCalls_;
    return std::format(
        "slices={} guest_avg_us={} guest_max_us={} host_avg_us={} host_max_us={} "
        "recent_guest_avg_us={} recent_guest_max_us={} recent_host_avg_us={} "
        "recent_host_max_us={} "
        "callbacks={} callback_avg_us={} callback_max_us={} yields={} "
        "yield_arg0_avg={} yield_arg0_max={} imports_per_frame={:.1f}",
        runSlices_, averageUs(guestRunTimeNs_, runSlices_), longestGuestRunNs_ / 1'000,
        averageUs(hostServiceTimeNs_, runSlices_), longestHostServiceNs_ / 1'000,
        averageUs(recentGuestTotal, recentRuntimeTimeCount_), recentGuestMax / 1'000,
        averageUs(recentHostTotal, recentRuntimeTimeCount_), recentHostMax / 1'000, callbackRuns_,
        averageUs(callbackTimeNs_, callbackRuns_), longestCallbackNs_ / 1'000, yieldCalls_,
        averageYieldArgument, longestYieldArgument_, importsPerFrame);
}

std::string Runtime::voiceDiagnostics() const {
    const std::string backend = audioBackend_ ? audioBackend_->voiceDiagnostics() : "unavailable";
    return std::format(
        "enabled={} requested={} unit={} start={}/{} capture={}/{} capture_nonzero={} "
        "capture_callbacks={} render_callbacks={} render_nonzero={} output={}/{}; host {}",
        voiceChatEnabled_ ? 1 : 0, audioUnitStartRequested_ ? 1 : 0, audioUnitStarted_ ? 1 : 0,
        audioUnitStartAttempts_ - audioUnitStartFailures_, audioUnitStartAttempts_,
        audioUnitCaptureChunks_, audioUnitCaptureSamples_, audioUnitCaptureNonSilentChunks_,
        audioUnitCaptureCallbacks_, audioUnitRenderCallbacks_, audioUnitRenderNonSilentCallbacks_,
        audioUnitOutputQueued_, audioUnitOutputRejected_, backend);
}

std::string_view Runtime::importName(GuestAddr trapAddr) const {
    if (trapAddr >= kExtTrapBase && trapAddr <= kExtTrapEnd) {
        return "<ext>";
    }
    const uint32_t importId = trapToSymbolIndex(trapAddr);
    return importId < symbols_.size() ? std::string_view(symbols_[importId])
                                      : std::string_view("(bad id)");
}

void Runtime::bindImportHandlers() {
    boundHandlers_.clear();
    boundHandlers_.resize(symbols_.size());
    for (std::size_t index = 0; index < symbols_.size(); ++index) {
        if (const auto handler = handlers_.find(symbols_[index]); handler != handlers_.end()) {
            boundHandlers_[index] = handler->second;
        }
    }
}

void Runtime::logf(const char *fmt, ...) const {
    if (!config_.debugLog) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    __android_log_print(ANDROID_LOG_INFO, "codboz", "%s", buf);
#else
    std::vfprintf(stderr, fmt, ap);
    std::fflush(stderr);
#endif
    va_end(ap);
}

void Runtime::dispatch(GuestAddr trapAddr, GuestCpu &cpu) {
    ++importCount_;
    if (stopRequested_.load(std::memory_order_relaxed)) {
        cpu.halt();
        return;
    }

    recentImports_[recentImportCursor_] = trapAddr;
    recentImportCursor_ = (recentImportCursor_ + 1) % recentImports_.size();
    recentImportCount_ = std::min(recentImportCount_ + 1, recentImports_.size());

    if (trapAddr >= kExtTrapBase && trapAddr <= kExtTrapEnd) {
        auto it = extHandlers_.find(trapAddr);
        recordImport(trapAddr, it != extHandlers_.end() ? "<ext>" : "<ext:unknown>");
        reportProgress(cpu, it != extHandlers_.end() ? "<ext>" : "<ext:unknown>");
        if (config_.debugLog && loggedExtensionImports_.insert(trapAddr).second) {
            logf("[new #%llu] ext 0x%08x\n", static_cast<unsigned long long>(importCount_),
                 trapAddr);
        }
        if (config_.importCap != 0 && importCount_ >= config_.importCap) {
            cpu.halt();
            return;
        }
        if (it != extHandlers_.end()) {
            it->second(cpu);
        } else {
            cpu.setReg(0, 0);
        }
        return;
    }

    uint32_t importId = trapToSymbolIndex(trapAddr);
    const std::string *name = importId < symbols_.size() ? &symbols_[importId] : nullptr;
    const std::string_view nameView = name ? std::string_view(*name) : std::string_view("(bad id)");
    recordImport(trapAddr, nameView);
    reportProgress(cpu, nameView);
    if (config_.debugLog && importId < loggedImports_.size() && !loggedImports_[importId]) {
        loggedImports_[importId] = 1;
        logf("[new #%llu] %s\n", static_cast<unsigned long long>(importCount_), name->c_str());
    }
    if (config_.importCap != 0 && importCount_ >= config_.importCap) {
        cpu.halt();
        return;
    }
    if (importId < boundHandlers_.size() && boundHandlers_[importId]) {
        boundHandlers_[importId](cpu);
        return;
    }
    logf("[UNHANDLED] %.*s r0=%08x r1=%08x r2=%08x r3=%08x\n", static_cast<int>(nameView.size()),
         nameView.data(), cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    cpu.setReg(0, 0);
}

void Runtime::registerHandlers() {
    auto noop = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    auto ret64 = [this](GuestCpu &cpu) {
        uint64_t v = elapsedMs();
        GuestArguments::setU64Return(cpu, v);
    };
    auto printStr = [this](int reg, const char *tag) {
        return [this, reg, tag](GuestCpu &cpu) {
            GuestAddr p = cpu.reg(reg);
            if (p != 0) {
                logf("[%s] %s\n", tag, mem_.readString(p, 1024).c_str());
            }
            cpu.setReg(0, 0);
        };
    };

    handlers_["s3eDebugTraceLine"] = printStr(0, "trace");
    handlers_["s3eDebugOutputString"] = printStr(0, "dbg");
    handlers_["s3eDebugPrint"] = printStr(1, "print");
    handlers_["s3eDebugErrorShow"] = printStr(1, "ERROR");
    handlers_["s3eDebugAssertShow"] = noop;
    handlers_["s3eDebugGetInt"] = noop;
    handlers_["s3eDebugIsDebuggerPresent"] = noop;

    handlers_["s3eMemoryGetInt"] = [](GuestCpu &cpu) {
        // The reference reports the full device memory amount for every key.
        cpu.setReg(0, uint32_t(768u * 1024u * 1024u));
    };
    handlers_["s3eMemorySetInt"] = [this](GuestCpu &cpu) {
        if (memSetInts_.insert(cpu.reg(0)).second) {
            logf("[memSetInt] key=%u value=%d\n", cpu.reg(0), static_cast<int32_t>(cpu.reg(1)));
        }
        cpu.setReg(0, 0);
    };
    for (const char *n : {"s3eMemoryGetError", "s3eMemoryGetErrorString", "s3eMemorySetUserMemMgr",
                          "s3eMemoryGetUserMemMgr"}) {
        handlers_[n] = noop;
    }
    // Explicit heaps: the game creates heap 0 (128 MiB) for large resource
    // buffers and smaller heaps for other subsystems. Allocate from the guest
    // heap and return the base address.
    handlers_["s3eMemoryHeapCreate"] = [this](GuestCpu &cpu) {
        uint32_t idx = cpu.reg(0);
        if (idx >= 8) {
            cpu.setReg(0, 1); // S3E_RESULT_ERROR
            return;
        }
        if (heapBases_[idx] == 0) {
            uint32_t sz = (idx == 0) ? (128u * 1024u * 1024u) : (16u * 1024u * 1024u);
            GuestAddr base = heap_.alloc(sz);
            if (base == 0) {
                logf("[heapCreate FAIL idx=%u sz=%u]\n", idx, sz);
                cpu.setReg(0, 1);
                return;
            }
            heapBases_[idx] = base;
            heapSizes_[idx] = sz;
            logf("[heapCreate] idx=%u base=0x%08x sz=%u\n", idx, base, sz);
        }
        cpu.setReg(0, 0); // S3E_RESULT_SUCCESS
    };
    handlers_["s3eMemoryHeapDestroy"] = [this](GuestCpu &cpu) {
        uint32_t idx = cpu.reg(0);
        if (idx < 8 && heapBases_[idx] != 0) {
            heap_.free(heapBases_[idx]);
            heapBases_[idx] = 0;
            heapSizes_[idx] = 0;
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3eMemoryHeapAddress"] = [this](GuestCpu &cpu) {
        uint32_t idx = cpu.reg(0);
        cpu.setReg(0, (idx < 8) ? heapBases_[idx] : 0);
    };

    auto readStr = [this](GuestCpu &cpu, int reg) {
        GuestAddr p = cpu.reg(reg);
        return p ? mem_.readString(p, 128) : std::string("(null)");
    };
    handlers_["s3eConfigGetInt"] = [this, readStr](GuestCpu &cpu) {
        std::string section = readStr(cpu, 0);
        std::string key = readStr(cpu, 1);
        GuestAddr out = cpu.reg(2);
        int32_t v = 0;
        bool found = s3eConfig_.getInt(section, key, v);
        if (config_.debugLog && seenLog_.insert("cfg:" + section + "/" + key).second) {
            logf("[cfgGetInt] %s/%s %s\n", section.c_str(), key.c_str(),
                 found ? "(found)" : "(nf)");
        }
        if (found && out) {
            mem_.write32(out, static_cast<uint32_t>(v));
        }
        cpu.setReg(0, found ? 0 : 1);
    };
    handlers_["s3eConfigGetString"] = [this, readStr](GuestCpu &cpu) {
        std::string section = readStr(cpu, 0);
        std::string key = readStr(cpu, 1);
        GuestAddr out = cpu.reg(2);
        const std::string *v = s3eConfig_.find(section, key);
        if (config_.debugLog && seenLog_.insert("cfg:" + section + "/" + key).second) {
            logf("[cfgGetStr] %s/%s = '%s'\n", section.c_str(), key.c_str(),
                 v ? v->c_str() : "(nf)");
        }
        if (v && out) {
            mem_.writeString(out, *v);
        }
        cpu.setReg(0, v ? 0 : 1);
    };
    handlers_["s3eDeviceGetInt"] = [](GuestCpu &cpu) {
        uint32_t key = cpu.reg(0);
        uint32_t v = 0;
        if (key == 0) {
            v = 0x12;
        } else if (key == 10) {
            v = 0x00080101;
        }
        cpu.setReg(0, v);
    };
    handlers_["s3eDeviceGetString"] = [this](GuestCpu &cpu) {
        uint32_t key = cpu.reg(0);
        const char *s = "";
        switch (key) {
        case 0:
            s = "Android";
            break;
        case 2:
            s = xperiaControls_.load(std::memory_order_acquire) ? "R800i"
                                                                : config_.deviceModel.c_str();
            break;
        case 8:
            s = "ARM7A";
            break;
        case 0x0d:
            s = "4.1.2";
            break;
        case 0x13:
            s = "8.6";
            break;
        case 0x14:
            s = "en_US";
            break;
        case 0x15:
            s = xperiaControls_.load(std::memory_order_acquire) ? "Sony Ericsson Xperia Play"
                                                                : config_.deviceModel.c_str();
            break;
        case 0x19:
            s = "00000000000000000000000000000000";
            break;
        default:
            s = "";
            break;
        }
        cpu.setReg(0, internString(s));
    };
    for (const char *n : {"s3eDeviceCheckQuitRequest", "s3eDeviceCheckPauseRequest",
                          "s3eDeviceRegister", "s3eDeviceUnRegister", "s3eDeviceSetInt",
                          "s3eDeviceBacklightOn", "s3eDeviceRequestQuit"}) {
        handlers_[n] = noop;
    }
    auto deviceYield = [this](GuestCpu &cpu) {
        const uint32_t argument = cpu.reg(0);
        ++yieldCalls_;
        yieldArgumentSum_ += argument;
        longestYieldArgument_ = std::max(longestYieldArgument_, argument);
        pumpPointerEvents();
        const uint64_t value = elapsedMs();
        GuestArguments::setU64Return(cpu, value);
        if (!runningGuestCallback_) {
            cpu.halt();
        }
    };
    handlers_["s3eDeviceYield"] = deviceYield;
    handlers_["s3eDeviceYieldUntilEvent"] = deviceYield;

    handlers_["s3eTimerGetMs"] = ret64;
    handlers_["s3eTimerGetUST"] = ret64;
    handlers_["s3eTimerGetUTC"] = [](GuestCpu &cpu) {
        const uint64_t value =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
        GuestArguments::setU64Return(cpu, value);
    };
    handlers_["s3eTimerGetInt"] = [](GuestCpu &cpu) {
        cpu.setReg(0, cpu.reg(0) == 0 ? 1u : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eTimerSetTimer"] = [this](GuestCpu &cpu) {
        const uint32_t periodMs = cpu.reg(0);
        const GuestAddr function = cpu.reg(1);
        const GuestAddr userData = cpu.reg(2);
        auto existing = std::find_if(timers_.begin(), timers_.end(), [&](const auto &timer) {
            return timer.function == function && timer.userData == userData;
        });
        if (!function || (existing == timers_.end() && timers_.size() >= 32)) {
            cpu.setReg(0, 1);
            return;
        }
        TimerEvent timer{std::chrono::steady_clock::now() + std::chrono::milliseconds(periodMs),
                         function, userData, ++timerSequence_};
        if (existing == timers_.end()) {
            timers_.push_back(timer);
        } else {
            *existing = timer;
        }
        logf("[timer] set period=%u fn=0x%08x user=0x%08x\n", periodMs, function, userData);
        cpu.setReg(0, 0);
    };
    handlers_["s3eTimerCancelTimer"] = [this](GuestCpu &cpu) {
        const GuestAddr function = cpu.reg(0);
        const GuestAddr userData = cpu.reg(1);
        const auto before = timers_.size();
        std::erase_if(timers_, [&](const auto &timer) {
            return timer.function == function && timer.userData == userData;
        });
        cpu.setReg(0, before == timers_.size() ? 1u : 0u);
    };
    handlers_["s3eTimerGetLocaltimeOffset"] = [](GuestCpu &cpu) {
        cpu.setReg(0, 0);
        cpu.setReg(1, 0);
    };

    handlers_["s3eAudioPlay"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, playMusic(cpu.reg(0), cpu.reg(1)));
    };
    handlers_["s3eAudioStop"] = [this](GuestCpu &cpu) {
        if (audioBackend_) {
            audioBackend_->stopMusic();
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3eAudioResume"] = [this](GuestCpu &cpu) {
        if (audioBackend_) {
            audioBackend_->resumeMusic();
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3eAudioPause"] = [this](GuestCpu &cpu) {
        if (audioBackend_) {
            audioBackend_->pauseMusic();
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3eAudioSetInt"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, setAudioInt(cpu.reg(0), static_cast<int32_t>(cpu.reg(1))));
    };
    handlers_["s3eAudioGetInt"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(getAudioInt(cpu.reg(0))));
    };
    handlers_["s3eAudioIsPlaying"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, getAudioInt(1) == 1 ? 1u : 0u);
    };

    handlers_["s3eSoundGetInt"] = [this](GuestCpu &cpu) {
        const uint32_t key = cpu.reg(0);
        uint32_t value = std::numeric_limits<uint32_t>::max();
        switch (key) {
        case 0:
            value = static_cast<uint32_t>(soundVolume_);
            break;
        case 1:
            value = static_cast<uint32_t>(soundOutputRate_);
            break;
        case 2:
            value = static_cast<uint32_t>(soundRate_);
            break;
        case 3:
            value = 24;
            break;
        case 5:
        case 7:
            value = 1;
            break;
        default:
            break;
        }
        logf("[audio] soundGetInt key=%u value=%d\n", key, static_cast<int32_t>(value));
        cpu.setReg(0, value);
    };
    handlers_["s3eSoundSetInt"] = [this](GuestCpu &cpu) {
        const uint32_t key = cpu.reg(0);
        const int32_t value = static_cast<int32_t>(cpu.reg(1));
        logf("[audio] soundSetInt key=%u value=%d\n", key, value);
        if (key == 0) {
            soundVolume_ = std::clamp(value, 0, 256);
            if (audioBackend_) {
                for (int32_t channel = 0; channel < static_cast<int32_t>(soundChannels_.size());
                     ++channel) {
                    const auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
                    audioBackend_->setSoundVolume(channel, slot.volume * soundVolume_ / 256);
                }
            }
            cpu.setReg(0, 0);
        } else if (key == 2) {
            soundRate_ = std::clamp(value, 0, 0x40000);
            for (auto &slot : soundChannels_) {
                if (!slot.playing && !slot.decoding) {
                    slot.rate = soundRate_;
                }
            }
            cpu.setReg(0, 0);
        } else {
            cpu.setReg(0, 1);
        }
    };
    handlers_["s3eSoundGetFreeChannel"] = [this](GuestCpu &cpu) {
        serviceSoundChannels();
        int32_t freeChannel = -1;
        const uint32_t active = audioBackend_ ? audioBackend_->activeSoundMask() : 0;
        for (int32_t channel = 0; channel < static_cast<int32_t>(soundChannels_.size());
             ++channel) {
            const auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
            if (!slot.playing && !slot.decoding && (active & (1u << channel)) == 0) {
                freeChannel = channel;
                break;
            }
        }
        cpu.setReg(0, static_cast<uint32_t>(freeChannel));
    };
    handlers_["s3eSoundChannelRegister"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        const uint32_t type = cpu.reg(1);
        const GuestAddr function = cpu.reg(2);
        logf("[audio] channelRegister channel=%d type=%u function=0x%08x user=0x%08x\n", channel,
             type, function, cpu.reg(3));
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size()) || type >= 4 ||
            type == 3 || !function) {
            cpu.setReg(0, 1);
            return;
        }
        soundChannels_[static_cast<std::size_t>(channel)].callbacks[type] = {function, cpu.reg(3)};
        cpu.setReg(0, 0);
    };
    handlers_["s3eSoundChannelUnRegister"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        const uint32_t type = cpu.reg(1);
        logf("[audio] channelUnregister channel=%d type=%u\n", channel, type);
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size()) || type >= 4) {
            cpu.setReg(0, 1);
            return;
        }
        soundChannels_[static_cast<std::size_t>(channel)].callbacks[type] = {};
        cpu.setReg(0, 0);
    };
    handlers_["s3eSoundChannelPlay"] = [this](GuestCpu &cpu) {
        int32_t channel = static_cast<int32_t>(cpu.reg(0));
        const GuestAddr data = cpu.reg(1);
        const uint32_t samples = cpu.reg(2);
        const uint32_t repeat = cpu.reg(3);
        const uint32_t loopIndex = cpu.stackArg(0);
        logf("[audio] channelPlay channel=%d data=0x%08x samples=%u repeat=%u loop=%u\n", channel,
             data, samples, repeat, loopIndex);
        serviceSoundChannels();
        if (channel < 0) {
            const uint32_t active = audioBackend_ ? audioBackend_->activeSoundMask() : 0;
            for (int32_t candidate = 0; candidate < static_cast<int32_t>(soundChannels_.size());
                 ++candidate) {
                const auto &slot = soundChannels_[static_cast<std::size_t>(candidate)];
                if (!slot.playing && !slot.decoding && (active & (1u << candidate)) == 0) {
                    channel = candidate;
                    break;
                }
            }
        }
        const bool validData = samples <= std::numeric_limits<uint32_t>::max() / sizeof(int16_t) &&
                               mem_.isMapped(data, samples * sizeof(int16_t));
        const bool started = audioBackend_ && validData &&
                             beginSoundDecode(channel, data, samples, repeat, loopIndex, false);
        cpu.setReg(0, started ? 0u : 1u);
        if (started && soundChannels_[static_cast<std::size_t>(channel)].decoding &&
            !runningGuestCallback_) {
            cpu.halt();
        }
    };
    handlers_["s3eSoundChannelStop"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size())) {
            cpu.setReg(0, 1);
            return;
        }
        finishSoundChannel(channel, true);
        cpu.setReg(0, 0);
    };
    handlers_["s3eSoundChannelPause"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size()) ||
            !audioBackend_) {
            cpu.setReg(0, 1);
            return;
        }
        audioBackend_->pauseSound(channel);
        soundChannels_[static_cast<std::size_t>(channel)].paused = true;
        cpu.setReg(0, 0);
    };
    handlers_["s3eSoundChannelResume"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size()) ||
            !audioBackend_) {
            cpu.setReg(0, 1);
            return;
        }
        audioBackend_->resumeSound(channel);
        soundChannels_[static_cast<std::size_t>(channel)].paused = false;
        cpu.setReg(0, 0);
    };
    handlers_["s3eSoundChannelSetInt"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        const uint32_t key = cpu.reg(1);
        const int32_t value = static_cast<int32_t>(cpu.reg(2));
        logf("[audio] channelSetInt channel=%d key=%u value=%d\n", channel, key, value);
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size())) {
            cpu.setReg(0, 1);
            return;
        }
        auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
        switch (key) {
        case 0: {
            slot.rateScale = value;
            const int64_t scaled = static_cast<int64_t>(soundRate_) * (value >> 8);
            slot.rate = std::clamp<int32_t>(static_cast<int32_t>((scaled + 0xff) >> 8), 0, 0x40000);
            cpu.setReg(0, 0);
            break;
        }
        case 1:
            slot.rate = std::clamp(value, 0, 0x40000);
            cpu.setReg(0, 0);
            break;
        case 2:
            slot.userValue = value;
            cpu.setReg(0, 0);
            break;
        case 3:
            slot.volume = std::clamp(value, 0, 256);
            if (audioBackend_) {
                audioBackend_->setSoundVolume(channel, slot.volume * soundVolume_ / 256);
            }
            cpu.setReg(0, 0);
            break;
        default:
            cpu.setReg(0, 1);
            break;
        }
    };
    handlers_["s3eSoundChannelGetInt"] = [this](GuestCpu &cpu) {
        const int32_t channel = static_cast<int32_t>(cpu.reg(0));
        const uint32_t key = cpu.reg(1);
        if (channel < 0 || channel >= static_cast<int32_t>(soundChannels_.size())) {
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            return;
        }
        serviceSoundChannels();
        const auto &slot = soundChannels_[static_cast<std::size_t>(channel)];
        int32_t value = -1;
        switch (key) {
        case 0:
            value = slot.rateScale;
            break;
        case 1:
            value = slot.rate;
            break;
        case 2:
            value = slot.userValue;
            break;
        case 3:
            value = slot.decoding ? 256 : slot.volume;
            break;
        case 4:
            value = slot.playing || slot.decoding ? 1 : 0;
            break;
        case 5:
            value = slot.paused ? 1 : 0;
            break;
        default:
            break;
        }
        cpu.setReg(0, static_cast<uint32_t>(value));
    };

    auto readAddress = [this](GuestAddr address) {
        guest::InetAddress result{};
        if (address) {
            const auto bytes = mem_.read(address, sizeof(result));
            std::memcpy(&result, bytes.data(), sizeof(result));
        }
        return result;
    };
    auto writeAddress = [this](GuestAddr address, const guest::InetAddress &value) {
        if (address) {
            mem_.write(address, reinterpret_cast<const uint8_t *>(&value), sizeof(value));
        }
    };
    handlers_["s3eInetHtonl"] = [](GuestCpu &cpu) {
        cpu.setReg(0, SocketSystem::hostToNetwork32(cpu.reg(0)));
    };
    handlers_["s3eInetNtohl"] = [](GuestCpu &cpu) {
        cpu.setReg(0, SocketSystem::networkToHost32(cpu.reg(0)));
    };
    handlers_["s3eInetHtons"] = [](GuestCpu &cpu) {
        cpu.setReg(0, SocketSystem::hostToNetwork16(static_cast<uint16_t>(cpu.reg(0))));
    };
    handlers_["s3eInetNtohs"] = [](GuestCpu &cpu) {
        cpu.setReg(0, SocketSystem::networkToHost16(static_cast<uint16_t>(cpu.reg(0))));
    };
    handlers_["s3eInetAton"] = [this](GuestCpu &cpu) {
        const GuestAddr out = cpu.reg(0);
        const GuestAddr text = cpu.reg(1);
        uint32_t address = 0;
        if (!out || !text || !SocketSystem::parseIpv4(mem_.readString(text, 128), address)) {
            sockets_.setError(1);
            cpu.setReg(0, 1);
            return;
        }
        mem_.write32(out, address);
        sockets_.setError(0);
        cpu.setReg(0, 0);
    };
    handlers_["s3eInetNtoa"] = [this](GuestCpu &cpu) {
        const std::string text = SocketSystem::formatIpv4(cpu.reg(0));
        GuestAddr out = cpu.reg(1);
        const int32_t length = static_cast<int32_t>(cpu.reg(2));
        const bool allocated = out == 0;
        if (allocated) {
            out = heap_.alloc(static_cast<GuestSize>(text.size() + 1));
        }
        if (!out ||
            (!allocated && (length <= 0 || static_cast<std::size_t>(length) <= text.size()))) {
            sockets_.setError(1);
            cpu.setReg(0, 0);
            return;
        }
        mem_.writeString(out, text);
        sockets_.setError(0);
        cpu.setReg(0, out);
    };
    handlers_["s3eInetToString"] = [this, readAddress](GuestCpu &cpu) {
        if (!cpu.reg(0)) {
            sockets_.setError(1);
            cpu.setReg(0, 0);
            return;
        }
        const auto value = readAddress(cpu.reg(0));
        const std::string text = SocketSystem::formatAddress(value, cpu.reg(1) != 0);
        if (text.empty()) {
            sockets_.setError(1);
            cpu.setReg(0, 0);
            return;
        }
        sockets_.setError(0);
        cpu.setReg(0, internString(text));
    };
    handlers_["s3eInetLookup"] = [this, readAddress, writeAddress](GuestCpu &cpu) {
        const GuestAddr hostname = cpu.reg(0);
        const GuestAddr addressPointer = cpu.reg(1);
        const GuestAddr callback = cpu.reg(2);
        if (!hostname || !addressPointer) {
            sockets_.setError(1);
            cpu.setReg(0, 1);
            return;
        }
        auto address = readAddress(addressPointer);
        const std::string requestedName = mem_.readString(hostname, 512);
        const std::string name = controlConfig_.resolveHostname(requestedName);
        if (name != requestedName) {
            logf("[socket] route host='%s' to configured multiplayer server\n",
                 requestedName.c_str());
        }
        if (callback) {
            if (!sockets_.startLookup(name, address.port)) {
                cpu.setReg(0, 1);
                return;
            }
            pendingLookup_ = PendingLookup{addressPointer, callback, cpu.reg(3)};
            logf("[socket] async lookup host='%s'\n", name.c_str());
            cpu.setReg(0, 0);
            return;
        }
        const int32_t result = sockets_.lookup(
            name, address, [this] { return stopRequested_.load(std::memory_order_acquire); });
        if (result == 0) {
            writeAddress(addressPointer, address);
        }
        logf("[socket] lookup host='%s' result=%d error=%d\n", name.c_str(), result,
             sockets_.error());
        cpu.setReg(0, static_cast<uint32_t>(result));
    };
    handlers_["s3eInetLookupCancel"] = [this](GuestCpu &cpu) {
        sockets_.cancelLookup();
        pendingLookup_.reset();
        cpu.setReg(0, 0);
    };
    handlers_["s3eSocketCreate"] = [this](GuestCpu &cpu) {
        const uint32_t handle = sockets_.create(cpu.reg(0), static_cast<int32_t>(cpu.reg(1)));
        logf("[socket] create type=%u domain=%d handle=%u error=%d\n", cpu.reg(0),
             static_cast<int32_t>(cpu.reg(1)), handle, sockets_.error());
        cpu.setReg(0, handle);
    };
    handlers_["s3eSocketClose"] = [this](GuestCpu &cpu) {
        const uint32_t handle = cpu.reg(0);
        const int32_t result = sockets_.close(handle);
        logf("[socket] close handle=%u result=%d error=%d\n", handle, result, sockets_.error());
        cpu.setReg(0, static_cast<uint32_t>(result));
    };
    handlers_["s3eSocketBind"] = [this, readAddress](GuestCpu &cpu) {
        std::optional<guest::InetAddress> address;
        if (cpu.reg(1))
            address = readAddress(cpu.reg(1));
        const int32_t result =
            sockets_.bind(cpu.reg(0), address ? &*address : nullptr, cpu.reg(2) != 0);
        logf("[socket] bind handle=%u port=%u reuse=%u result=%d error=%d\n", cpu.reg(0),
             address ? SocketSystem::networkToHost16(address->port) : 0u, cpu.reg(2), result,
             sockets_.error());
        cpu.setReg(0, static_cast<uint32_t>(result));
    };
    handlers_["s3eSocketListen"] = [this](GuestCpu &cpu) {
        const uint32_t handle = cpu.reg(0);
        const uint16_t backlog = static_cast<uint16_t>(cpu.reg(1));
        const int32_t result = sockets_.listen(handle, backlog);
        logf("[socket] listen handle=%u backlog=%u result=%d error=%d\n", handle, backlog, result,
             sockets_.error());
        cpu.setReg(0, static_cast<uint32_t>(result));
    };
    handlers_["s3eSocketAccept"] = [this, writeAddress](GuestCpu &cpu) {
        guest::InetAddress address{};
        const uint32_t accepted =
            sockets_.accept(cpu.reg(0), cpu.reg(1) ? &address : nullptr, {cpu.reg(2), cpu.reg(3)});
        if (accepted && cpu.reg(1))
            writeAddress(cpu.reg(1), address);
        cpu.setReg(0, accepted);
    };
    handlers_["s3eSocketConnect"] = [this, readAddress](GuestCpu &cpu) {
        if (!cpu.reg(1)) {
            sockets_.setError(1);
            cpu.setReg(0, 1);
            return;
        }
        const uint32_t handle = cpu.reg(0);
        const auto address = readAddress(cpu.reg(1));
        const int32_t result = sockets_.connect(handle, address, {cpu.reg(2), cpu.reg(3)});
        logf("[socket] connect handle=%u type=%d port=%u callback=%u result=%d error=%d\n", handle,
             address.type, SocketSystem::networkToHost16(address.port), cpu.reg(2) != 0, result,
             sockets_.error());
        cpu.setReg(0, static_cast<uint32_t>(result));
    };
    handlers_["s3eSocketSend"] = [this](GuestCpu &cpu) {
        const uint32_t length = cpu.reg(2);
        if (!cpu.reg(1) && length) {
            sockets_.setError(1);
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            return;
        }
        const auto data = mem_.read(cpu.reg(1), length);
        cpu.setReg(0, static_cast<uint32_t>(
                          sockets_.send(cpu.reg(0), data, static_cast<int32_t>(cpu.reg(3)))));
    };
    handlers_["s3eSocketSendTo"] = [this, readAddress](GuestCpu &cpu) {
        const uint32_t length = cpu.reg(2);
        const GuestAddr address = cpu.stackArg(0);
        if ((!cpu.reg(1) && length) || !address) {
            sockets_.setError(1);
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            return;
        }
        const auto data = mem_.read(cpu.reg(1), length);
        cpu.setReg(0,
                   static_cast<uint32_t>(sockets_.sendTo(
                       cpu.reg(0), data, static_cast<int32_t>(cpu.reg(3)), readAddress(address))));
    };
    handlers_["s3eSocketRecv"] = [this](GuestCpu &cpu) {
        if (!cpu.reg(1) && cpu.reg(2)) {
            sockets_.setError(1);
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            return;
        }
        const auto result =
            sockets_.receive(cpu.reg(0), cpu.reg(2), static_cast<int32_t>(cpu.reg(3)), false);
        if (result.count >= 0 && !result.data.empty()) {
            mem_.write(cpu.reg(1), result.data.data(), static_cast<GuestSize>(result.data.size()));
        }
        cpu.setReg(0, static_cast<uint32_t>(result.count));
    };
    handlers_["s3eSocketRecvFrom"] = [this, writeAddress](GuestCpu &cpu) {
        if (!cpu.reg(1) && cpu.reg(2)) {
            sockets_.setError(1);
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            return;
        }
        const auto result =
            sockets_.receive(cpu.reg(0), cpu.reg(2), static_cast<int32_t>(cpu.reg(3)), true);
        if (result.count >= 0 && !result.data.empty()) {
            mem_.write(cpu.reg(1), result.data.data(), static_cast<GuestSize>(result.data.size()));
        }
        const GuestAddr address = cpu.stackArg(0);
        if (result.source && address)
            writeAddress(address, *result.source);
        cpu.setReg(0, static_cast<uint32_t>(result.count));
    };
    handlers_["s3eSocketReadable"] = [this](GuestCpu &cpu) {
        cpu.setReg(0,
                   static_cast<uint32_t>(sockets_.readable(cpu.reg(0), {cpu.reg(1), cpu.reg(2)})));
    };
    handlers_["s3eSocketWritable"] = [this](GuestCpu &cpu) {
        cpu.setReg(0,
                   static_cast<uint32_t>(sockets_.writable(cpu.reg(0), {cpu.reg(1), cpu.reg(2)})));
    };
    handlers_["s3eSocketGetInt"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(sockets_.getInt(cpu.reg(0))));
    };
    handlers_["s3eSocketGetError"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(sockets_.error()));
    };
    handlers_["s3eSocketGetString"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, internString(sockets_.getString(cpu.reg(0))));
    };
    handlers_["s3eSocketGetLocalName"] = [this, writeAddress](GuestCpu &cpu) {
        if (!cpu.reg(1)) {
            sockets_.setError(1);
            cpu.setReg(0, 1);
            return;
        }
        guest::InetAddress address{};
        const int32_t result = sockets_.localName(cpu.reg(0), address);
        if (result == 0)
            writeAddress(cpu.reg(1), address);
        cpu.setReg(0, static_cast<uint32_t>(result));
    };
    handlers_["s3eSocketGetPeerName"] = [this, writeAddress](GuestCpu &cpu) {
        if (!cpu.reg(1)) {
            sockets_.setError(1);
            cpu.setReg(0, 1);
            return;
        }
        guest::InetAddress address{};
        const int32_t result = sockets_.peerName(cpu.reg(0), address);
        if (result == 0)
            writeAddress(cpu.reg(1), address);
        cpu.setReg(0, static_cast<uint32_t>(result));
    };

    handlers_["s3eMallocBase"] = [this](GuestCpu &cpu) {
        uint32_t size = cpu.reg(0);
        GuestAddr a = heap_.alloc(size ? size : 1);
        if (a == 0) {
            logf("[malloc FAIL size=%u]\n", size);
        } else if (size >= 65536 && bigAllocs_.insert(size).second) {
            logf("[malloc big size=%u -> 0x%08x]\n", size, a);
        }
        cpu.setReg(0, a);
    };
    handlers_["s3eFreeBase"] = [this](GuestCpu &cpu) {
        heap_.free(cpu.reg(0));
        cpu.setReg(0, 0);
    };
    handlers_["s3eReallocBase"] = [this](GuestCpu &cpu) {
        uint32_t ptr = cpu.reg(0);
        uint32_t size = cpu.reg(1);
        GuestAddr result = heap_.realloc(ptr, size ? size : 1);
        if (result == 0) {
            logf("[realloc FAIL] ptr=0x%08x size=%u\n", ptr, size);
        }
        cpu.setReg(0, result);
    };

    handlers_["s3ePointerRegister"] = [this](GuestCpu &cpu) {
        const uint32_t id = cpu.reg(0);
        const GuestAddr function = cpu.reg(1);
        if (id < pointerCallbacks_.size() && function) {
            auto &callbacks = pointerCallbacks_[id];
            callbacks.add(function, cpu.reg(2));
            logf("[pointer] register id=%u fn=0x%08x user=0x%08x count=%zu\n", id, function,
                 cpu.reg(2), callbacks.size());
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3ePointerUnRegister"] = [this](GuestCpu &cpu) {
        const uint32_t id = cpu.reg(0);
        if (id < pointerCallbacks_.size()) {
            auto &callbacks = pointerCallbacks_[id];
            const GuestAddr function = cpu.reg(1);
            callbacks.remove(function);
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3ePointerUpdate"] = [this](GuestCpu &cpu) {
        publishPointerState();
        clearPointerTransitions();
        if (!runningGuestCallback_) {
            pointerTransitionPending_ = false;
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3ePointerGetInt"] = [](GuestCpu &cpu) {
        const uint32_t key = cpu.reg(0);
        uint32_t value = 0;
        switch (key) {
        case 0:
            value = 1;
            break;
        case 1:
            value = 0;
            break;
        case 2:
        case 3:
            value = 2;
            break;
        case 4:
            value = 1;
            break;
        default:
            break;
        }
        cpu.setReg(0, value);
    };
    handlers_["s3ePointerGetState"] = [this](GuestCpu &cpu) {
        const uint32_t button = cpu.reg(0);
        const uint32_t state = button == 0 ? pointerPublishedState_ : 0;
        cpu.setReg(0, state);
    };
    handlers_["s3ePointerGetX"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(pointerPublishedX_));
    };
    handlers_["s3ePointerGetY"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(pointerPublishedY_));
    };
    handlers_["s3ePointerGetTouchState"] = [this](GuestCpu &cpu) {
        const int32_t id = static_cast<int32_t>(cpu.reg(0));
        const auto touch = publishedTouchPoints_.find(id);
        const uint32_t state = touch == publishedTouchPoints_.end() ? 0 : touch->second.state;
        cpu.setReg(0, state);
    };
    handlers_["s3ePointerGetTouchX"] = [this](GuestCpu &cpu) {
        const auto touch = publishedTouchPoints_.find(static_cast<int32_t>(cpu.reg(0)));
        cpu.setReg(
            0, touch == publishedTouchPoints_.end() ? 0 : static_cast<uint32_t>(touch->second.x));
    };
    handlers_["s3ePointerGetTouchY"] = [this](GuestCpu &cpu) {
        const auto touch = publishedTouchPoints_.find(static_cast<int32_t>(cpu.reg(0)));
        cpu.setReg(
            0, touch == publishedTouchPoints_.end() ? 0 : static_cast<uint32_t>(touch->second.y));
    };
    handlers_["s3ePointerGetPressure"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, cpu.reg(0) == 0 && pointerPublishedDown_ ? 1 : 0);
    };
    handlers_["s3ePointerGetTouchPressure"] = [this](GuestCpu &cpu) {
        const auto touch = publishedTouchPoints_.find(static_cast<int32_t>(cpu.reg(0)));
        cpu.setReg(0, touch != publishedTouchPoints_.end() && touch->second.down ? 1 : 0);
    };
    handlers_["s3ePointerGetError"] = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    handlers_["s3ePointerGetErrorString"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, internString("S3E_POINTER_ERR_NONE"));
    };
    handlers_["s3ePointerSetInt"] = [this](GuestCpu &cpu) {
        const uint32_t key = cpu.reg(0);
        logf("[pointer] set-int key=%u value=%d\n", key, static_cast<int32_t>(cpu.reg(1)));
        // The Android view already converts touches to guest display coordinates. Applying the
        // original raw-display transform here would transform the same event twice.
        cpu.setReg(0, key >= 5 && key <= 12 ? 0 : 1);
    };

    handlers_["s3eVideoGetInt"] = [](GuestCpu &cpu) {
        switch (cpu.reg(0)) {
        case 0:
        case 1:
            cpu.setReg(0, 256);
            break;
        case 2:
        case 3:
        case 0x28:
            cpu.setReg(0, 0);
            break;
        default:
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            break;
        }
    };
    for (const char *name : {"s3eVideoPlay", "s3eVideoStop", "s3eVideoResume"}) {
        handlers_[name] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    }

    handlers_["s3eKeyboardRegister"] = [this](GuestCpu &cpu) {
        const uint32_t id = cpu.reg(0);
        const GuestAddr function = cpu.reg(1);
        auto existing = std::find_if(keyboardCallbacks_.begin(), keyboardCallbacks_.end(),
                                     [id, function](const auto &callback) {
                                         return callback.function == function && callback.id == id;
                                     });
        if (existing != keyboardCallbacks_.end()) {
            existing->userData = cpu.reg(2);
        } else {
            auto empty = std::find_if(keyboardCallbacks_.begin(), keyboardCallbacks_.end(),
                                      [](const auto &callback) { return !callback.function; });
            if (empty != keyboardCallbacks_.end()) {
                *empty = {id, function, cpu.reg(2)};
            }
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3eKeyboardUnRegister"] = [this](GuestCpu &cpu) {
        const uint32_t id = cpu.reg(0);
        const GuestAddr function = cpu.reg(1);
        for (auto &callback : keyboardCallbacks_) {
            if (callback.function && callback.id == id &&
                (!function || callback.function == function)) {
                callback = {};
            }
        }
        cpu.setReg(0, 0);
    };
    handlers_["s3eKeyboardUpdate"] = [this](GuestCpu &cpu) {
        publishKeyboardState();
        cpu.setReg(0, 0);
        if (!runningGuestCallback_ && !pendingGuestCallbacks_.empty()) {
            cpu.halt();
        }
    };
    handlers_["s3eKeyboardGetState"] = [this](GuestCpu &cpu) {
        const uint32_t key = cpu.reg(0);
        uint8_t state = key < keyboardPublished_.size() ? keyboardPublished_[key] : 0;
        if (key < controllerUiPublished_.size() &&
            controllerBridge_.isFrontendKeyboardQuery(cpu.reg(14))) {
            const GuestAddr manager = cpu.reg(4);
            if (controllerBridge_.validFrontendManager(mem_, manager)) {
                frontendInputManager_ = manager;
                controllerUiActive_ = controllerBridge_.frontendActive(mem_, manager);
            }
            state |= controllerUiPublished_[key];
        }
        cpu.setReg(0, state);
    };
    handlers_["s3eKeyboardAnyKey"] = [this](GuestCpu &cpu) {
        auto key = std::find_if(keyboardPublished_.begin(), keyboardPublished_.end(),
                                [](uint8_t state) { return state & kInputPressed; });
        cpu.setReg(0, key == keyboardPublished_.end()
                          ? 0
                          : static_cast<uint32_t>(key - keyboardPublished_.begin()));
    };
    handlers_["s3eKeyboardGetInt"] = [](GuestCpu &cpu) {
        switch (cpu.reg(0)) {
        case 0:
        case 2:
            cpu.setReg(0, 1);
            break;
        case 1:
        case 4:
        case 5:
        case 6:
            cpu.setReg(0, 0);
            break;
        default:
            cpu.setReg(0, std::numeric_limits<uint32_t>::max());
            break;
        }
    };
    handlers_["s3eKeyboardClearState"] = [this](GuestCpu &cpu) {
        clearKeyboardState();
        cpu.setReg(0, 0);
    };
    handlers_["s3eKeyboardSetInt"] = noop;
    handlers_["s3eKeyboardGetDisplayName"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, internString(""));
    };

    for (const char *n :
         {"s3eAccelerometerStart", "s3eAccelerometerStop", "s3eAccelerometerGetX",
          "s3eAccelerometerGetY", "s3eAccelerometerGetZ", "s3eAccelerometerGetInt"}) {
        handlers_[n] = noop;
    }

    handlers_["s3eSurfaceGetInt"] = [this](GuestCpu &cpu) {
        uint32_t key = cpu.reg(0);
        uint32_t v = 0;
        switch (key) {
        case 0:
            v = config_.display.width;
            break; // width
        case 1:
            v = config_.display.height;
            break; // height
        case 2:
            v = config_.display.pitch();
            break; // pitch
        case 3:
            v = kGuestDisplayBytesPerPixel;
            break; // bytes per pixel
        case 11:
            v = 4;
            break;
        default:
            break;
        }
        cpu.setReg(0, v);
    };
    for (const char *n : {"s3eSurfaceRegister", "s3eSurfaceUnRegister", "s3eSurfaceSetup",
                          "s3eSurfaceShow", "s3eGLRegister", "s3eGLUnRegister", "s3eRegisterNoop",
                          "s3eTouchpadGetInt", "s3eTouchpadRegister", "s3eTouchpadUnRegister"}) {
        handlers_[n] = noop;
    }

    handlers_["s3eSurfacePtr"] = [this](GuestCpu &cpu) { cpu.setReg(0, surfaceBuffer_); };
    handlers_["s3eGLGetInt"] = [this](GuestCpu &cpu) {
        switch (cpu.reg(0)) {
        case 0:
            cpu.setReg(0, config_.display.width);
            break;
        case 1:
            cpu.setReg(0, config_.display.height);
            break;
        default:
            cpu.setReg(0, 0);
            break;
        }
    };
    handlers_["s3eGLGetNativeWindow"] = [this](GuestCpu &cpu) { cpu.setReg(0, nativeWindowAddr_); };

    handlers_["eglGetDisplay"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglInitialize"] = [](GuestCpu &cpu) {
        // minor/major version out-params at r1, r2
        if (cpu.reg(1)) {
            /* host would write version */
        }
        cpu.setReg(0, 1); // EGL_TRUE
    };
    handlers_["eglChooseConfig"] = [this](GuestCpu &cpu) {
        // r3 = configs out, stack[0] = num_config out
        if (cpu.reg(3)) {
            mem_.write32(cpu.reg(3), 1); // one fake config
        }
        cpu.setReg(0, 1);
    };
    handlers_["eglCreateContext"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglCreateWindowSurface"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglCreatePbufferSurface"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglMakeCurrent"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglSwapBuffers"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglBindAPI"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglBindTexImage"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglReleaseTexImage"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglDestroyContext"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglDestroySurface"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglTerminate"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglGetError"] = [](GuestCpu &cpu) { cpu.setReg(0, 0x3000); }; // EGL_SUCCESS
    handlers_["eglGetCurrentContext"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglGetCurrentDisplay"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglGetCurrentSurface"] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    handlers_["eglGetConfigs"] = [this](GuestCpu &cpu) {
        if (cpu.reg(1)) {
            mem_.write32(cpu.reg(1), 1);
        }
        cpu.setReg(0, 1);
    };
    handlers_["eglGetConfigAttrib"] = [this](GuestCpu &cpu) {
        if (cpu.reg(3)) {
            mem_.write32(cpu.reg(3), 8); // some reasonable value
        }
        cpu.setReg(0, 1);
    };
    handlers_["eglQuerySurface"] = [this](GuestCpu &cpu) {
        if (cpu.reg(3)) {
            constexpr uint32_t kEglHeight = 0x3056;
            constexpr uint32_t kEglWidth = 0x3057;
            uint32_t value = 0;
            if (cpu.reg(2) == kEglWidth)
                value = config_.display.width;
            if (cpu.reg(2) == kEglHeight)
                value = config_.display.height;
            mem_.write32(cpu.reg(3), value);
        }
        cpu.setReg(0, 1);
    };
    handlers_["eglQueryContext"] = [this](GuestCpu &cpu) {
        if (cpu.reg(3)) {
            mem_.write32(cpu.reg(3), 0);
        }
        cpu.setReg(0, 1);
    };
    handlers_["eglQueryString"] = [this](GuestCpu &cpu) { cpu.setReg(0, internString("")); };
    // eglGetProcAddress: return a trap address that acts as a no-op stub
    // (returns 0). This mirrors the reference's make_stub/s3e_trampoline_dispatch.
    // Without it the game stores NULL and crashes when it later calls the
    // extension function pointer.
    handlers_["eglGetProcAddress"] = [this, readStr](GuestCpu &cpu) {
        [[maybe_unused]] std::string name = readStr(cpu, 0);
        GuestAddr trap = addExtension([](GuestCpu &c) { c.setReg(0, 0); });
        cpu.setReg(0, trap);
    };

    handlers_["glGetError"] = [](GuestCpu &cpu) { cpu.setReg(0, 0); }; // GL_NO_ERROR
    handlers_["glCheckFramebufferStatus"] = [](GuestCpu &cpu) {
        cpu.setReg(0, 0x8CD5); // GL_FRAMEBUFFER_COMPLETE
    };
    handlers_["glGetString"] = [this](GuestCpu &cpu) {
        uint32_t name = cpu.reg(0);
        const char *s = "";
        switch (name) {
        case 0x1F00:
            s = "Qualcomm";
            break; // GL_VENDOR
        case 0x1F01:
            s = "Adreno 205";
            break; // GL_RENDERER
        case 0x1F02:
            s = "OpenGL ES 2.0";
            break;   // GL_VERSION
        case 0x1F03: // GL_EXTENSIONS
            s = "GL_OES_texture_npot GL_OES_compressed_ETC1_RGB8_texture "
                "GL_OES_depth_texture GL_OES_depth24 GL_OES_rgb8_rgba8 "
                "GL_OES_vertex_half_float GL_OES_element_index_uint "
                "GL_OES_fragment_precision_high GL_OES_standard_derivatives "
                "GL_EXT_texture_format_BGRA8888 GL_EXT_blend_minmax";
            break;
        case 0x8B8C:
            s = "OpenGL ES GLSL ES 1.00";
            break; // GL_SHADING_LANGUAGE_VERSION
        default:
            break;
        }
        cpu.setReg(0, internString(s));
    };
    auto genIds = [this](GuestCpu &cpu) {
        GuestAddr out = cpu.reg(1);
        uint32_t n = cpu.reg(0);
        for (uint32_t i = 0; i < n && i < 16; ++i) {
            mem_.write32(out + i * 4, nextGenName_++);
        }
    };
    handlers_["glGenTextures"] = genIds;
    handlers_["glGenBuffers"] = genIds;
    handlers_["glGenFramebuffers"] = genIds;
    handlers_["glGenRenderbuffers"] = genIds;
    handlers_["glGenVertexArrays"] = genIds;
    handlers_["glGenQueries"] = genIds;
    handlers_["glGenSamplers"] = genIds;
    handlers_["glGenProgramPipelines"] = genIds;
    nextGenName_ = 1;
    for (const char *n :
         {"glIsTexture", "glIsBuffer", "glIsFramebuffer", "glIsRenderbuffer", "glIsProgram",
          "glIsShader", "glIsQuery", "glIsVertexArray", "glIsSampler", "glIsSync"}) {
        handlers_[n] = [](GuestCpu &cpu) { cpu.setReg(0, 1); };
    }
    handlers_["glGetIntegerv"] = [this](GuestCpu &cpu) {
        uint32_t pname = cpu.reg(0);
        GuestAddr params = cpu.reg(1);
        if (!params)
            return;
        auto w1 = [&](uint32_t v) { mem_.write32(params, v); };
        auto w2 = [&](uint32_t a, uint32_t b) {
            mem_.write32(params, a);
            mem_.write32(params + 4, b);
        };
        switch (pname) {
        case 0x0D33:
            w1(4096);
            break; // GL_MAX_TEXTURE_SIZE
        case 0x0D3A:
            w2(4096, 4096);
            break; // GL_MAX_VIEWPORT_DIMS
        case 0x0D52:
            w1(8);
            break; // GL_RED_BITS
        case 0x0D53:
            w1(8);
            break; // GL_GREEN_BITS
        case 0x0D54:
            w1(8);
            break; // GL_BLUE_BITS
        case 0x0D55:
            w1(8);
            break; // GL_ALPHA_BITS
        case 0x0D56:
            w1(24);
            break; // GL_DEPTH_BITS
        case 0x0D57:
            w1(8);
            break; // GL_STENCIL_BITS
        case 0x84BF:
            w1(4096);
            break; // GL_MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x846D:
            w1(4);
            break; // GL_MAX_ELEMENTS_INDICES (approx)
        case 0x8089:
            w1(4);
            break; // GL_MAX_ELEMENTS_VERTICES (approx)
        case 0x8869:
            w1(16);
            break; // GL_MAX_VERTEX_ATTRIBS
        case 0x8DFB:
            w1(128);
            break; // GL_MAX_VERTEX_UNIFORM_VECTORS
        case 0x8DFC:
            w1(8);
            break; // GL_MAX_VARYING_VECTORS
        case 0x8DFD:
            w1(16);
            break; // GL_MAX_FRAGMENT_UNIFORM_VECTORS
        case 0x8B4D:
            w1(8);
            break; // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
        case 0x8872:
            w1(8);
            break; // GL_MAX_TEXTURE_IMAGE_UNITS
        case 0x8B4C:
            w1(0);
            break; // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
        case 0x86A2:
            w1(1);
            break; // GL_NUM_COMPRESSED_TEXTURE_FORMATS
        case 0x8B4A:
            w1(16);
            break; // GL_MAX_VERTEX_UNIFORM_COMPONENTS (fallback)
        default:
            w1(0);
            break;
        }
    };
    handlers_["glGetBooleanv"] = [this](GuestCpu &cpu) {
        if (cpu.reg(1)) {
            mem_.write8(cpu.reg(1), 1);
        }
    };
    handlers_["glGetFloatv"] = [this](GuestCpu &cpu) {
        if (cpu.reg(1)) {
            mem_.write32(cpu.reg(1), 0);
        }
    };
    handlers_["glCreateShader"] = [this](GuestCpu &cpu) { cpu.setReg(0, nextGenName_++); };
    handlers_["glCreateProgram"] = [this](GuestCpu &cpu) { cpu.setReg(0, nextGenName_++); };
    handlers_["glGetAttribLocation"] = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    handlers_["glGetUniformLocation"] = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    handlers_["glGetShaderiv"] = [this](GuestCpu &cpu) {
        if (cpu.reg(2)) {
            mem_.write32(cpu.reg(2), 1); // GL_TRUE for compile status
        }
    };
    handlers_["glGetProgramiv"] = [this](GuestCpu &cpu) {
        if (cpu.reg(2)) {
            mem_.write32(cpu.reg(2), 1); // GL_TRUE for link status
        }
    };
    handlers_["glGetActiveUniform"] = noop;
    handlers_["glGetQueryiv"] = noop;
    handlers_["glGetQueryObjectuiv"] = [this](GuestCpu &cpu) {
        if (cpu.reg(3)) {
            mem_.write32(cpu.reg(3), 0);
        }
    };
    handlers_["glReadPixels"] = noop;
    handlers_["glShaderSource"] = noop;

    // Catch-all: every remaining gl*/egl* import is a no-op so the game's GL
    // state commands proceed without crashing. These are the integer/enum-only
    // functions (glEnable, glDisable, glClear, glViewport, etc.).
    for (const auto &sym : symbols_) {
        if ((sym.starts_with("gl") || sym.starts_with("egl")) &&
            handlers_.find(sym) == handlers_.end()) {
            handlers_[sym] = noop;
        }
    }

    handlers_["s3eFileOpen"] = [this, readStr](GuestCpu &cpu) {
        std::string name = readStr(cpu, 0);
        std::string mode = readStr(cpu, 1);
        GuestHandle h = fs_.open(name, mode);
        logf("[file] open(%s, %s) -> 0x%08x\n", name.c_str(), mode.c_str(), h);
        cpu.setReg(0, h);
    };
    handlers_["s3eFileClose"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, fs_.close(cpu.reg(0)) ? 0u : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileRead"] = [this](GuestCpu &cpu) {
        GuestAddr buf = cpu.reg(0);
        uint32_t elem = cpu.reg(1);
        uint32_t count = cpu.reg(2);
        GuestHandle h = cpu.reg(3);
        const uint64_t requested = static_cast<uint64_t>(elem) * count;
        if (requested == 0 || requested > std::numeric_limits<uint32_t>::max() || buf == 0 ||
            !mem_.isMapped(buf, static_cast<uint32_t>(requested))) {
            cpu.setReg(0, 0);
            return;
        }
        constexpr uint32_t kChunkSize = 1024u * 1024u;
        const uint32_t total = static_cast<uint32_t>(requested);
        std::vector<uint8_t> chunk(std::min(total, kChunkSize));
        uint32_t completed = 0;
        while (completed < total) {
            const uint32_t next = std::min<uint32_t>(chunk.size(), total - completed);
            const uint32_t read = fs_.read(chunk.data(), next, h);
            if (read == 0) {
                break;
            }
            mem_.write(buf + completed, chunk.data(), read);
            completed += read;
            if (read < next) {
                break;
            }
        }
        cpu.setReg(0, completed / elem);
    };
    handlers_["s3eFileWrite"] = [this](GuestCpu &cpu) {
        const GuestAddr buf = cpu.reg(0);
        const uint32_t elem = cpu.reg(1);
        const uint32_t count = cpu.reg(2);
        const GuestHandle handle = cpu.reg(3);
        const uint64_t requested = static_cast<uint64_t>(elem) * count;
        if (requested == 0 || requested > std::numeric_limits<uint32_t>::max() || buf == 0 ||
            !mem_.isMapped(buf, static_cast<uint32_t>(requested))) {
            cpu.setReg(0, 0);
            return;
        }
        constexpr uint32_t kChunkSize = 1024u * 1024u;
        const uint32_t total = static_cast<uint32_t>(requested);
        uint32_t completed = 0;
        while (completed < total) {
            const uint32_t next = std::min(kChunkSize, total - completed);
            const auto data = mem_.read(buf + completed, next);
            const uint32_t written = fs_.write(data.data(), next, handle);
            completed += written;
            if (written < next) {
                break;
            }
        }
        cpu.setReg(0, completed / elem);
    };
    handlers_["s3eFileCheckExists"] = [this, readStr](GuestCpu &cpu) {
        std::string name = readStr(cpu, 0);
        bool ex = fs_.exists(name);
        logf("[file] exists(%s) -> %s\n", name.c_str(), ex ? "yes" : "no");
        cpu.setReg(0, ex ? 1 : 0);
    };
    handlers_["s3eFileGetSize"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(fs_.size(cpu.reg(0))));
    };
    handlers_["s3eFileTell"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(fs_.tell(cpu.reg(0))));
    };
    handlers_["s3eFileSeek"] = [this](GuestCpu &cpu) {
        const bool result = fs_.seek(cpu.reg(0), static_cast<int32_t>(cpu.reg(1)),
                                     static_cast<int32_t>(cpu.reg(2)));
        cpu.setReg(0, result ? 0u : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileGetChar"] = [this](GuestCpu &cpu) {
        uint8_t ch = 0;
        uint32_t n = fs_.read(&ch, 1, cpu.reg(0));
        cpu.setReg(0, n > 0 ? ch : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFilePutChar"] = [this](GuestCpu &cpu) {
        const uint8_t value = static_cast<uint8_t>(cpu.reg(0));
        cpu.setReg(0, fs_.write(&value, 1, cpu.reg(1)) == 1 ? value
                                                            : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileFlush"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, fs_.flush(cpu.reg(0)) ? 0u : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileGetError"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, static_cast<uint32_t>(fs_.error()));
    };
    handlers_["s3eFileGetErrorString"] = [this](GuestCpu &cpu) {
        cpu.setReg(0, internString(fs_.errorString()));
    };
    handlers_["s3eFileMakeDirectory"] = [this, readStr](GuestCpu &cpu) {
        cpu.setReg(0,
                   fs_.makeDirectory(readStr(cpu, 0)) ? 0u : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileDelete"] = [this, readStr](GuestCpu &cpu) {
        cpu.setReg(0, fs_.remove(readStr(cpu, 0)) ? 0u : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileRename"] = [this, readStr](GuestCpu &cpu) {
        cpu.setReg(0, fs_.rename(readStr(cpu, 0), readStr(cpu, 1))
                          ? 0u
                          : std::numeric_limits<uint32_t>::max());
    };
    handlers_["s3eFileOpenFromMemory"] = [this](GuestCpu &cpu) {
        const GuestAddr address = cpu.reg(0);
        const uint32_t size = cpu.reg(1);
        if (address == 0 || size == 0 || !mem_.isMapped(address, size)) {
            cpu.setReg(0, 0);
            return;
        }
        cpu.setReg(0, fs_.openMemory(mem_.read(address, size)));
    };
    handlers_["s3eFileGetFileInt"] = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    handlers_["s3eFileListDirectory"] = [](GuestCpu &cpu) { cpu.setReg(0, 0); };
    handlers_["s3eFileListClose"] = noop;
    handlers_["s3eFileAddUserFileSys"] = noop;

    // s3eExtGetHash(hash, iface, size): fill a function-pointer table for a known
    // extension hash with trap addresses, or return 1 (unsupported).
    handlers_["s3eExtGetHash"] = [this](GuestCpu &cpu) {
        uint32_t hash = cpu.reg(0);
        GuestAddr iface = cpu.reg(1);
        uint32_t size = cpu.reg(2);
        auto it = extTables_.find(hash);
        if (it == extTables_.end() ||
            (hash == kHashTouchpad && !xperiaControls_.load(std::memory_order_acquire))) {
            logf("[ext] lookup hash=0x%08x size=%u result=unsupported\n", hash, size);
            cpu.setReg(0, 1);
            return;
        }
        const auto &traps = it->second;
        if (size < traps.size() * 4u) {
            logf("[ext] lookup hash=0x%08x size=%u required=%zu result=too-small\n", hash, size,
                 traps.size() * 4u);
            cpu.setReg(0, 1);
            return;
        }
        for (std::size_t i = 0; i < traps.size(); ++i) {
            mem_.write32(iface + static_cast<GuestAddr>(i) * 4u, traps[i]);
        }
        logf("[ext] lookup hash=0x%08x size=%u slots=%zu result=ok\n", hash, size, traps.size());
        cpu.setReg(0, 0);
    };
}

Runtime::BootResult Runtime::boot(const S3eImage &image) {
    if (!matchesSourceImportInventory(image.symbols())) {
        throw std::runtime_error("guest import inventory does not match the supported source");
    }
    auto loaded = loadAndRelocate(mem_, image);
    if (!controllerBridge_.initialize(mem_, loaded.base)) {
        throw std::runtime_error("unsupported controller action sites");
    }
    const GuestAddr playerName = internString(controlConfig_.playerName());
    if (!applyPlayerName(mem_, loaded.base, playerName)) {
        throw std::runtime_error("unsupported player-name patch site");
    }
    if (!CodbozFrameInterpolation::apply(mem_, loaded.base)) {
        throw std::runtime_error("unsupported frame-interpolation patch site");
    }
    mem_.mapRegion(0x70000000u, 0x100000u); // stack
    mem_.mapRegion(0x61000000u, 0x100000u); // GL string arena
    mem_.mapRegion(kCallbackScratch, kCallbackScratchSize + 0x2000u);
    surfaceBuffer_ = 0x40000000u;
    mem_.mapRegion(surfaceBuffer_, config_.display.bufferSize());
    nativeWindowAddr_ = surfaceBuffer_ + config_.display.bufferSize() + 256u;
    mem_.mapRegion(nativeWindowAddr_, 0x10u);
    mem_.write16(nativeWindowAddr_, static_cast<uint16_t>(config_.display.width));
    mem_.write16(nativeWindowAddr_ + 2, static_cast<uint16_t>(config_.display.height));

    auto cfg = image.config();
    s3eConfig_.parse({reinterpret_cast<const char *>(cfg.data()), cfg.size()});
    s3eConfig_.set("GAME", "ResourceDownloader", "0");
    s3eConfig_.set("GAME", "EnableAndroidMarketBilling", "0");
    s3eConfig_.set("RESMANAGER", "ResBuildStyle", fs_.resourceStyle());
    applyControlConfig(s3eConfig_, controlConfig_);
    s3eConfig_.set("GAME", "VoiceChatEnabled", voiceChatEnabled_ ? "1" : "0");

    const auto recoveryPoints = CodbozFixups::recoveryPoints(loaded.base);
    {
        std::lock_guard lock(cpuMutex_);
        cpu_ = std::make_unique<DynarmicCpu>(
            mem_, [this](GuestAddr trapAddr, GuestCpu &c) { dispatch(trapAddr, c); },
            recoveryPoints);
    }
    cpu_->syncPageTable();
    cpu_->setReg(13, 0x700FFF00u);
    cpu_->setReg(14, kCallbackReturnTrap);
    cpu_->setThumb(false);
    cpu_->setPc(loaded.entry);

    // Initialise the per-PC fault fixups for the loaded image.
    fixups_.init(mem_, loaded.base);
    logf("[BOOT] image base=0x%08x entry=0x%08x\n", loaded.base, loaded.entry);

    RunOutcome outcome = RunOutcome::Halted;
    uint64_t runCount = 0;
    GuestAddr lastWatchdogPc = 0;
    uint64_t lastWatchdogImports = 0;
    for (;;) {
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        const auto guestStart = std::chrono::steady_clock::now();
        outcome = cpu_->run();
        const auto guestDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - guestStart)
                                       .count();
        const auto guestDurationNs = static_cast<uint64_t>(std::max<int64_t>(guestDuration, 0));
        guestRunTimeNs_ += guestDurationNs;
        longestGuestRunNs_ = std::max(longestGuestRunNs_, guestDurationNs);
        recentGuestRunTimesNs_[recentRuntimeTimeCursor_] = guestDurationNs;
        ++runCount;
        ++runSlices_;
        const auto hostStart = std::chrono::steady_clock::now();
        serviceLifecycle();
        queueDueTimers();
        queueSocketEvents();
        queueZeroConfEvents();
        serviceSoundChannels();
        serviceAudioUnit();
        if (outcome != RunOutcome::Fault && !pendingGuestCallbacks_.empty()) {
            const auto callbackStart = std::chrono::steady_clock::now();
            const RunOutcome callbackOutcome = runGuestCallbacks();
            const auto callbackDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - callbackStart)
                                              .count();
            const auto callbackDurationNs =
                static_cast<uint64_t>(std::max<int64_t>(callbackDuration, 0));
            ++callbackRuns_;
            callbackTimeNs_ += callbackDurationNs;
            longestCallbackNs_ = std::max(longestCallbackNs_, callbackDurationNs);
            if (callbackOutcome == RunOutcome::Fault) {
                outcome = callbackOutcome;
            }
        }
        const auto hostDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - hostStart)
                                      .count();
        const auto hostDurationNs = static_cast<uint64_t>(std::max<int64_t>(hostDuration, 0));
        hostServiceTimeNs_ += hostDurationNs;
        longestHostServiceNs_ = std::max(longestHostServiceNs_, hostDurationNs);
        recentHostServiceTimesNs_[recentRuntimeTimeCursor_] = hostDurationNs;
        recentRuntimeTimeCursor_ = (recentRuntimeTimeCursor_ + 1) % recentGuestRunTimesNs_.size();
        recentRuntimeTimeCount_ =
            std::min(recentRuntimeTimeCount_ + 1, recentGuestRunTimesNs_.size());
        if ((runCount & 0x3FFu) == 0) { // every 1024 slices
            GuestAddr curPc = cpu_->pc();
            if (curPc == lastWatchdogPc && importCount_ == lastWatchdogImports &&
                (config_.importCap == 0 || importCount_ < config_.importCap)) {
                logf("[WATCHDOG] stuck at PC=0x%08x imports=%llu\n", curPc,
                     (unsigned long long)importCount_);
            }
            lastWatchdogPc = curPc;
            lastWatchdogImports = importCount_;
            if ((runCount & 0xFFFFu) == 0) { // every 65536 slices
                logf("[WATCHDOG] PC=0x%08x imports=%llu elapsed=%llums\n", curPc,
                     (unsigned long long)importCount_, (unsigned long long)elapsedMs());
            }
        }
        if (outcome == RunOutcome::Fault) {
            GuestAddr pc = cpu_->pc();
            uint32_t regs[16];
            for (int i = 0; i < 16; ++i) {
                regs[i] = cpu_->reg(i);
            }
            if (fixups_.tryFixup(mem_, pc, regs)) {
                logf("[fixup] name=%s pc=0x%08x bucket=%llu null-buffer=%llu null-string=%llu\n",
                     CodbozFixups::name(fixups_.lastKind()), pc,
                     static_cast<unsigned long long>(fixups_.bucketCount()),
                     static_cast<unsigned long long>(fixups_.nullBufferCount()),
                     static_cast<unsigned long long>(fixups_.nullStringCount()));
                for (int i = 0; i < 16; ++i) {
                    cpu_->setReg(i, regs[i]);
                }
                // Set Thumb state from the interworking bit of the patched PC.
                cpu_->setThumb(regs[15] & 1u);
                cpu_->setPc(regs[15] & ~1u);
                outcome = RunOutcome::Halted; // resume
            } else {
                break;
            }
        }
        if (outcome == RunOutcome::CallbackReturned) {
            break;
        }
        if (stopRequested_.load(std::memory_order_acquire) ||
            (config_.importCap != 0 && importCount_ >= config_.importCap)) {
            break;
        }
    }

    BootResult r;
    r.outcome = outcome;
    r.importsHandled = importCount_;
    r.lastImport = lastImportTrap_ ? std::string(importName(lastImportTrap_)) : std::string();
    r.firstImports = firstImports_;
    if (outcome == RunOutcome::Fault) {
        GuestAddr fa = cpu_->lastFaultAddr();
        // Dump the last 20 imports before the fault.
        logf("[FAULT] recent imports:");
        const std::size_t first =
            recentImportCount_ < recentImports_.size() ? 0 : recentImportCursor_;
        for (std::size_t i = 0; i < recentImportCount_; ++i) {
            const auto name = importName(recentImports_[(first + i) % recentImports_.size()]);
            logf(" %.*s", static_cast<int>(name.size()), name.data());
        }
        logf("\n");
        logf("[FAULT] pc=0x%08x addr=0x%08x lr=0x%08x sp=0x%08x r0=%08x r1=%08x r2=%08x r3=%08x "
             "r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x\n",
             cpu_->pc(), fa, cpu_->reg(14), cpu_->reg(13), cpu_->reg(0), cpu_->reg(1), cpu_->reg(2),
             cpu_->reg(3), cpu_->reg(4), cpu_->reg(5), cpu_->reg(6), cpu_->reg(7), cpu_->reg(8),
             cpu_->reg(9), cpu_->reg(10), cpu_->reg(11), cpu_->reg(12));
        // Dump some guest stack words for context.
        GuestAddr sp = cpu_->reg(13);
        if (sp >= 0x70000000u && sp < 0x70100000u) {
            logf("[FAULT] stack:");
            for (int i = 0; i < 8; ++i) {
                logf(" %08x", mem_.read32(sp + i * 4));
            }
            logf("\n");
        }
        const GuestAddr operand = cpu_->reg(10);
        const bool operandMapped = operand != 0 && mem_.isMapped(operand, 0x24);
        const uint32_t operandType = operandMapped ? mem_.read32(operand) : 0;
        const uint32_t operandValue = operandMapped ? mem_.read32(operand + 0x20) : 0;
        const GuestAddr vmStack = cpu_->reg(4);
        const bool stackMapped = vmStack != 0 && mem_.isMapped(vmStack, 0x10);
        const uint32_t stackCount = stackMapped ? mem_.read32(vmStack + 0x0c) : 0;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "fault pc=0x%08x addr=0x%08x lr=0x%08x sp=0x%08x r0=0x%08x "
                      "r1=0x%08x r2=0x%08x r3=0x%08x r4=0x%08x r10=0x%08x "
                      "operand_mapped=%u operand_type=%u operand_value=0x%08x stack_count=%u",
                      cpu_->pc(), fa, cpu_->reg(14), cpu_->reg(13), cpu_->reg(0), cpu_->reg(1),
                      cpu_->reg(2), cpu_->reg(3), cpu_->reg(4), operand, operandMapped ? 1u : 0u,
                      operandType, operandValue, stackCount);
        r.faultDetail = buf;
    }
    return r;
}

} // namespace codboz
