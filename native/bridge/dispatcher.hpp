#pragma once

#include "bridge/callback_list.hpp"
#include "audio/audio_backend.hpp"
#include "cpu/guest_cpu.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_heap.hpp"
#include "guest/guest_handles.hpp"
#include "guest/guest_memory.hpp"
#include "loader/s3e_image.hpp"
#include "network/socket_system.hpp"
#include "network/zeroconf_backend.hpp"
#include "s3e/config.hpp"
#include "s3e/control_config.hpp"
#include "s3e/display.hpp"
#include "s3e/file_system.hpp"
#include "gl/gl_bridge.hpp"
#include "game/codboz_controller_bridge.hpp"
#include "game/codboz_fixups.hpp"

#include <chrono>
#include <atomic>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace codboz {

enum class ControllerAction : int32_t {
    Accept = 0,
    Back = 1,
    Sprint = 2,
    Reload = 3,
    ChangeWeapon = 4,
    TacticalGrenade = 5,
    Grenade = 6,
    Aim = 7,
    Fire = 8,
    Melee = 9,
    Pause = 10,
    DpadUp = 11,
    DpadDown = 12,
    DpadLeft = 13,
    DpadRight = 14,
    Count = 15,
};

class DynarmicCpu;
class ControllerInputTestAccess;
class Runtime {
  public:
    struct Progress {
        uint64_t imports = 0;
        GuestAddr pc = 0;
        uint64_t uptimeMs = 0;
        std::string lastImport;
        std::string renderer;
        std::string glVersion;
        std::string glDiagnostics;
        std::string runtimeDiagnostics;
        std::string voiceDiagnostics;
    };

    struct Config {
        GuestAddr heapBase = 0x50000000u;
        GuestSize heapSize = 256u * 1024u * 1024u;
        GuestAddr stringArena = 0x60000000u;
        GuestSize stringArenaSize = 64u * 1024u;
        uint64_t importCap = 0;
        bool debugLog = false;
        bool xperiaControls = false;
        GuestDisplay display;
        // Host path to the imported game data (the S3E virtual-filesystem root).
        std::string root;
        std::string saveRoot;
        std::string controlConfigPath;
        std::string deviceModel = "Android";
        std::function<void(const Progress &)> progressCallback;
    };

    Runtime(GuestMemory &mem, std::vector<std::string> symbols, Config config,
            std::unique_ptr<AudioBackend> audioBackend = {},
            std::unique_ptr<ZeroConfBackend> zeroConfBackend = {});
    ~Runtime();

    // Load and relocate an S3E image, then run from its entry until the import
    // cap, a fault, or a guest-requested quit.
    struct BootResult {
        RunOutcome outcome;
        uint64_t importsHandled;
        std::string lastImport;
        std::vector<std::string> firstImports; // first N imports, in order
        std::string faultDetail;
    };
    BootResult boot(const S3eImage &image);
    void requestStop();
    void setPaused(bool paused);
    bool attachSurface(void *nativeWindow);
    const std::string &hostRenderer() const {
        return glBridge_.renderer();
    }
    const std::string &hostGlVersion() const {
        return glBridge_.version();
    }
    std::string hostGlDiagnostics() const {
        return glBridge_.diagnostics();
    }
    std::string hostRuntimeDiagnostics() const {
        return runtimeDiagnostics();
    }
    std::string hostVoiceDiagnostics() const {
        return voiceDiagnostics();
    }
    void queuePointerEvent(int32_t action, int32_t touchId, int32_t x, int32_t y);
    void queueControllerButton(ControllerAction action, bool pressed);
    void queueControllerAxes(float leftX, float leftY, float rightX, float rightY);
    void setControllerConnected(bool connected);
    void releaseController();
    void queueZeroConfFound(uint32_t searchHandle, std::string serviceKey, std::string name,
                            std::string serviceType, std::string domain, std::string host,
                            uint16_t port, uint32_t ipv4Address,
                            std::vector<std::string> txtRecords);
    void queueZeroConfLost(uint32_t searchHandle, std::string serviceKey);
    void queueZeroConfFailure(uint32_t handle, bool publication);

    // Called by the CPU for each trap (import or extension-table address).
    void dispatch(GuestAddr trapAddr, GuestCpu &cpu);

    GuestMemory &memory() {
        return mem_;
    }
    GuestHeap &heap() {
        return heap_;
    }
    // Provide an Android native window for real EGL/GLES rendering. Must be
    // called before boot().
    bool setSurface(void *nativeWindow) {
        if (!glBridge_.init(nativeWindow)) {
            return false;
        }
        glBridge_.registerHandlers(handlers_, extHandlers_, mem_);
        bindImportHandlers();
        return true;
    }

  private:
    friend class ControllerInputTestAccess;
    friend class PointerInputTestAccess;

    using Handler = std::function<void(GuestCpu &)>;
    void registerHandlers();
    void registerExtensions();
    void bindImportHandlers();
    // Assign a stable trap address (in the extension range) to a host function.
    GuestAddr addExtension(Handler h);
    GuestAddr internString(std::string_view s);
    uint64_t elapsedMs() const;
    void recordImport(GuestAddr trapAddr, std::string_view name);
    void reportProgress(GuestCpu &cpu, std::string_view name);
    std::string runtimeDiagnostics() const;
    std::string voiceDiagnostics() const;
    std::string_view importName(GuestAddr trapAddr) const;
    void logf(const char *fmt, ...) const;
    void publishPointerState();
    void clearPointerTransitions();
    void pumpPointerEvents();
    void pumpControllerInput();
    bool queueGuestControllerAction(GuestControllerAction action, bool pressed);
    void publishKeyboardState();
    void clearKeyboardState();
    void queueDueTimers();
    void queueSocketEvents();
    void queueZeroConfEvents();
    void serviceSoundChannels();
    void serviceAudioUnit();
    void serviceLifecycle();
    void interruptGuest();
    bool beginSoundDecode(int32_t channel, GuestAddr data, uint32_t samples, uint32_t repeat,
                          uint32_t loopIndex, bool beginAtLoop);
    void queueSoundGeneratorChunk(int32_t channel, uint64_t generation);
    void finishSoundChannel(int32_t channel, bool explicitStop);
    uint32_t playMusic(GuestAddr filename, uint32_t repeat);
    uint32_t setAudioInt(uint32_t key, int32_t value);
    int32_t getAudioInt(uint32_t key) const;
    uint32_t startAudioUnit();
    bool activateAudioUnit();
    void stopAudioUnit();
    RunOutcome runGuestCallbacks();

    struct PointerCallback {
        GuestAddr function = 0;
        GuestAddr userData = 0;
    };
    struct KeyboardCallback {
        uint32_t id = 0;
        GuestAddr function = 0;
        GuestAddr userData = 0;
    };
    struct HostPointerEvent {
        int32_t action = 0;
        int32_t touchId = 0;
        int32_t x = 0;
        int32_t y = 0;
    };
    struct TouchPoint {
        int32_t x = 0;
        int32_t y = 0;
        uint8_t state = 0;
        bool down = false;
    };
    struct PendingGuestCallback {
        GuestAddr function = 0;
        GuestAddr userData = 0;
        std::vector<uint8_t> payload;
        std::function<bool()> valid;
        std::function<void(uint32_t)> completion;
        std::array<uint32_t, 4> arguments{};
        bool customArguments = false;
    };
    struct SoundCallback {
        GuestAddr function = 0;
        GuestAddr userData = 0;
    };
    struct SoundChannel {
        std::array<SoundCallback, 4> callbacks{};
        int32_t volume = 256;
        int32_t rate = 11000;
        int32_t rateScale = 0x10000;
        int32_t userValue = 0;
        GuestAddr sourceData = 0;
        uint32_t sourceSamples = 0;
        uint32_t repeat = 0;
        uint32_t loopIndex = 0;
        uint64_t generation = 0;
        bool beginAtLoop = false;
        bool decoding = false;
        bool playing = false;
        bool paused = false;
        std::vector<int16_t> decoded;
    };
    struct TimerEvent {
        std::chrono::steady_clock::time_point due;
        GuestAddr function = 0;
        GuestAddr userData = 0;
        uint64_t sequence = 0;
    };

    GuestMemory &mem_;
    std::vector<std::string> symbols_;
    Config config_;
    S3eConfig s3eConfig_;
    ControlConfig controlConfig_;
    S3eFileSystem fs_;
    GuestHeap heap_;
    std::unique_ptr<AudioBackend> audioBackend_;
    std::unique_ptr<ZeroConfBackend> zeroConfBackend_;
    std::unordered_map<std::string, Handler> handlers_;
    std::vector<Handler> boundHandlers_;
    std::unordered_map<std::string, GuestAddr> interned_;
    GuestAddr stringCursor_ = 0;

    // Extension function tables returned via s3eExtGetHash. Each hash maps to a
    // list of trap addresses (one per table slot); extHandlers_ maps those trap
    // addresses to host implementations.
    static constexpr GuestAddr kExtTrapBase = 0xFFE00000u;
    GuestAddr nextExtTrap_ = kExtTrapBase;
    std::unordered_map<uint32_t, std::vector<GuestAddr>> extTables_;
    std::unordered_map<GuestAddr, Handler> extHandlers_;
    std::vector<uint8_t> loggedImports_;
    std::unordered_set<GuestAddr> loggedExtensionImports_;
    std::set<std::string> seenLog_;
    std::set<uint32_t> bigAllocs_;  // large malloc sizes already logged
    std::set<uint32_t> memSetInts_; // distinct s3eMemorySetInt keys logged

    // Surface and GL stub state.
    GuestAddr surfaceBuffer_ = 0;
    GuestAddr nativeWindowAddr_ = 0; // fake native window address
    uint32_t nextGenName_ = 1;       // counter for glGen* IDs

    GlBridge glBridge_;
    CodbozControllerBridge controllerBridge_;
    CodbozFixups fixups_;
    GuestAddr heapBases_[8] = {}; // explicit s3eMemoryHeapCreate bases
    GuestSize heapSizes_[8] = {};

    std::unique_ptr<DynarmicCpu> cpu_;
    std::mutex cpuMutex_;
    std::chrono::steady_clock::time_point start_;

    uint64_t importCount_ = 0;
    std::atomic<bool> stopRequested_ = false;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    bool pauseRequested_ = false;
    bool detachRequested_ = false;
    void *pendingSurface_ = nullptr;
    std::mutex pointerMutex_;
    std::deque<HostPointerEvent> pointerEvents_;
    bool pointerTransitionPending_ = false;
    std::array<CallbackList<PointerCallback>, 4> pointerCallbacks_;
    std::array<KeyboardCallback, 8> keyboardCallbacks_;
    std::array<PointerCallback, 2> touchpadCallbacks_;
    std::deque<PendingGuestCallback> pendingGuestCallbacks_;
    bool runningGuestCallback_ = false;
    int32_t pointerX_ = 0;
    int32_t pointerY_ = 0;
    uint8_t pointerState_ = 0;
    bool pointerDown_ = false;
    int32_t pointerPublishedX_ = pointerX_;
    int32_t pointerPublishedY_ = pointerY_;
    uint8_t pointerPublishedState_ = 0;
    bool pointerPublishedDown_ = false;
    std::unordered_map<int32_t, int32_t> pointerTouchSlots_;
    std::unordered_map<int32_t, TouchPoint> touchPoints_;
    std::unordered_map<int32_t, TouchPoint> publishedTouchPoints_;
    std::mutex controllerMutex_;
    struct ControllerButtonEvent {
        ControllerAction action = ControllerAction::Accept;
        bool pressed = false;
    };
    std::deque<ControllerButtonEvent> controllerButtons_;
    std::array<float, 4> pendingControllerAxes_{};
    bool controllerAxesDirty_ = false;
    bool controllerReleasePending_ = false;
    std::array<bool, static_cast<std::size_t>(ControllerAction::Count)> controllerActions_{};
    std::array<bool, static_cast<std::size_t>(GuestControllerAction::Count)>
        guestControllerActions_{};
    std::atomic<bool> xperiaControls_ = false;
    std::array<float, 4> controllerAxes_{};
    std::array<uint8_t, 256> keyboardLive_{};
    std::array<uint8_t, 256> keyboardPublished_{};
    std::array<uint8_t, 256> controllerUiLive_{};
    std::array<uint8_t, 256> controllerUiPublished_{};
    GuestAddr frontendInputManager_ = 0;
    bool controllerUiActive_ = false;
    std::array<int32_t, 2> touchpadX_{};
    std::array<int32_t, 2> touchpadY_{};
    std::array<uint8_t, 2> touchpadState_{};
    std::array<bool, 2> touchpadActive_{};
    std::vector<TimerEvent> timers_;
    uint64_t timerSequence_ = 0;
    SocketSystem sockets_;
    struct PendingLookup {
        GuestAddr address = 0;
        GuestAddr callback = 0;
        GuestAddr userData = 0;
    };
    std::optional<PendingLookup> pendingLookup_;
    struct ZeroConfDiscovered {
        GuestAddr token = 0;
        std::string name;
        std::string serviceType;
        std::string domain;
        std::string host;
        uint16_t port = 0;
        uint32_t ipv4Address = 0;
        std::vector<std::string> txtRecords;
    };
    struct ZeroConfSearch {
        GuestAddr foundCallback = 0;
        GuestAddr updateCallback = 0;
        GuestAddr lostCallback = 0;
        GuestAddr userData = 0;
        std::unordered_map<std::string, ZeroConfDiscovered> services;
    };
    struct ZeroConfPublication {};
    enum class ZeroConfEventType { Found, Lost, SearchFailure, PublicationFailure };
    struct ZeroConfEvent {
        ZeroConfEventType type = ZeroConfEventType::SearchFailure;
        uint32_t handle = 0;
        std::string serviceKey;
        std::string name;
        std::string serviceType;
        std::string domain;
        std::string host;
        uint16_t port = 0;
        uint32_t ipv4Address = 0;
        std::vector<std::string> txtRecords;
    };
    HandleTable<ZeroConfSearch> zeroConfSearches_;
    HandleTable<ZeroConfPublication> zeroConfPublications_;
    std::mutex zeroConfMutex_;
    std::deque<ZeroConfEvent> zeroConfEvents_;
    std::array<SoundChannel, 24> soundChannels_{};
    int32_t musicVolume_ = 256;
    int32_t soundVolume_ = 256;
    int32_t soundRate_ = 11000;
    int32_t soundOutputRate_ = 44100;
    GuestAddr audioUnitCaptureCallback_ = 0;
    GuestAddr audioUnitRenderCallback_ = 0;
    bool voiceChatEnabled_ = false;
    bool audioUnitStartRequested_ = false;
    bool audioUnitStarted_ = false;
    bool audioUnitRenderPending_ = false;
    uint64_t audioUnitGeneration_ = 0;
    std::chrono::steady_clock::time_point nextAudioUnitRender_{};
    uint64_t audioUnitStartAttempts_ = 0;
    uint64_t audioUnitStartFailures_ = 0;
    uint64_t audioUnitCaptureChunks_ = 0;
    uint64_t audioUnitCaptureSamples_ = 0;
    uint64_t audioUnitCaptureNonSilentChunks_ = 0;
    uint64_t audioUnitCaptureCallbacks_ = 0;
    uint64_t audioUnitRenderCallbacks_ = 0;
    uint64_t audioUnitRenderNonSilentCallbacks_ = 0;
    uint64_t audioUnitOutputQueued_ = 0;
    uint64_t audioUnitOutputRejected_ = 0;
    static constexpr GuestAddr kCallbackScratch = 0x62000000u;
    static constexpr GuestSize kCallbackScratchSize = 0x2000u;
    static constexpr GuestAddr kSoundPcmScratch = kCallbackScratch + kCallbackScratchSize;
    static constexpr uint32_t kSoundGeneratorChunkSamples = 4096;
    static constexpr uint32_t kMaxDecodedSoundSamples = 32u * 1024u * 1024u;
    static constexpr uint32_t kMaxSoundExpansion = 64;
    GuestAddr lastImportTrap_ = 0;
    uint64_t nextProgressReportMs_ = 0;
    uint64_t nextProgressCheckImport_ = 0;
    uint64_t runSlices_ = 0;
    uint64_t guestRunTimeNs_ = 0;
    uint64_t longestGuestRunNs_ = 0;
    uint64_t hostServiceTimeNs_ = 0;
    uint64_t longestHostServiceNs_ = 0;
    uint64_t callbackRuns_ = 0;
    uint64_t callbackTimeNs_ = 0;
    uint64_t longestCallbackNs_ = 0;
    uint64_t yieldCalls_ = 0;
    uint64_t yieldArgumentSum_ = 0;
    uint32_t longestYieldArgument_ = 0;
    std::array<uint64_t, 120> recentGuestRunTimesNs_{};
    std::array<uint64_t, 120> recentHostServiceTimesNs_{};
    std::size_t recentRuntimeTimeCount_ = 0;
    std::size_t recentRuntimeTimeCursor_ = 0;
    std::vector<std::string> firstImports_;
    std::array<GuestAddr, 20> recentImports_{};
    std::size_t recentImportCursor_ = 0;
    std::size_t recentImportCount_ = 0;
};

} // namespace codboz
