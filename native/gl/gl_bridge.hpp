#pragma once

#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace codboz {

class GuestCpu;

class GlBridge {
  public:
    using Handler = std::function<void(GuestCpu &)>;

    GlBridge() = default;
    ~GlBridge();

    bool init(void *nativeWindow);
    bool attachSurface(void *nativeWindow);
    void suspendSurface();
    void swap();
    bool active() const {
        return active_;
    }
    const std::string &renderer() const {
        return renderer_;
    }
    const std::string &version() const {
        return version_;
    }
    uint64_t presentedFrames() const {
        return presentedFrames_;
    }
    std::string diagnostics() const;

    void registerHandlers(std::unordered_map<std::string, Handler> &handlers,
                          std::unordered_map<GuestAddr, Handler> &extHandlers, GuestMemory &mem);

  private:
    enum class StateMetric : std::size_t {
        Viewport,
        BindBuffer,
        Count,
    };

    struct RectState {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        bool valid = false;
    };

    bool active_ = false;
    void *display_ [[maybe_unused]] = nullptr;
    void *context_ [[maybe_unused]] = nullptr;
    void *surface_ [[maybe_unused]] = nullptr;
    void *fallbackSurface_ [[maybe_unused]] = nullptr;
    void *config_ [[maybe_unused]] = nullptr;
    void *nativeWindow_ [[maybe_unused]] = nullptr;
    std::string renderer_;
    std::string version_;
    std::string firstIssue_;
    uint32_t issueCount_ = 0;
    uint32_t pendingGlError_ = 0;
    uint64_t presentedFrames_ = 0;
    std::chrono::steady_clock::time_point lastPresent_{};
    uint64_t measuredFrameTimeNs_ = 0;
    uint64_t measuredFrameIntervals_ = 0;
    uint64_t shortestFrameTimeNs_ = 0;
    uint64_t longestFrameTimeNs_ = 0;
    uint64_t frameTimingDiscontinuities_ = 0;
    uint64_t swapTimeNs_ = 0;
    uint64_t swapCount_ = 0;
    uint64_t longestSwapTimeNs_ = 0;
    std::array<uint64_t, 120> recentFrameTimesNs_{};
    std::size_t recentFrameTimeCount_ = 0;
    std::size_t recentFrameTimeCursor_ = 0;
    uint32_t textureUploads_ = 0;
    uint32_t compressedTextureUploads_ = 0;
    uint32_t maxTextureWidth_ = 0;
    uint32_t maxTextureHeight_ = 0;
    uint32_t defaultViewportWidth_ = 0;
    uint32_t defaultViewportHeight_ = 0;
    uint32_t boundFramebuffer_ = 0;
    bool usedOffscreenFramebuffer_ = false;
    RectState lastDefaultViewport_;
    RectState lastOffscreenViewport_;
    RectState lastScissor_;
    bool scissorEnabled_ = false;
    std::array<uint64_t, static_cast<std::size_t>(StateMetric::Count)> stateCalls_{};
    std::array<uint64_t, static_cast<std::size_t>(StateMetric::Count)> redundantStateCalls_{};
    std::array<uint64_t, static_cast<std::size_t>(StateMetric::Count)> elidedStateCalls_{};
    uint64_t directClientAttributes_ = 0;
    uint64_t clientAttributeCopies_ = 0;
    uint64_t clientAttributeBytes_ = 0;
    uint64_t clientAttributeGrowths_ = 0;
    uint64_t directClientIndices_ = 0;
    uint64_t clientIndexCopies_ = 0;
    uint64_t clientIndexBytes_ = 0;
    uint64_t clientIndexGrowths_ = 0;
    uint64_t directGuestData_ = 0;
    uint64_t guestDataCopies_ = 0;
    uint64_t guestDataBytes_ = 0;
    uint64_t guestDataGrowths_ = 0;
    GuestAddr nextProcTrap_ = 0xFFE10000u;
    std::unordered_map<std::string, GuestAddr> procTraps_;

    void forwardSimple(std::unordered_map<std::string, Handler> &handlers, GuestMemory &mem);
    void recordStateCall(StateMetric metric, bool redundant, bool elided = false);
    void recordIssue(std::string issue);
    void captureGlError(const char *operation);
    void recordTextureUpload(int32_t level, int32_t width, int32_t height, bool compressed);
};

} // namespace codboz
