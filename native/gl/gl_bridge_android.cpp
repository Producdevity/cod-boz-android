#include "gl_bridge.hpp"

#include "cpu/guest_cpu.hpp"
#include "gl_compat.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

#define TAG "codboz-gl"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace codboz {

union FloatBits {
    uint32_t u;
    float f;
};

const uint8_t *readGuestData(GuestMemory &mem, GuestAddr address, GuestSize size,
                             std::vector<uint8_t> &scratch, uint64_t &directCount,
                             uint64_t &copyCount, uint64_t &copiedBytes,
                             uint64_t &capacityGrowths) {
    if (!address || !size) {
        return nullptr;
    }
    if (const auto *data = mem.contiguousData(address, size)) {
        ++directCount;
        return data;
    }
    if (size > scratch.capacity()) {
        ++capacityGrowths;
    }
    scratch.resize(size);
    if (!mem.copyOut(address, scratch.data(), size)) {
        return nullptr;
    }
    ++copyCount;
    copiedBytes += size;
    return scratch.data();
}

static float asFloat(uint32_t u) {
    FloatBits fb;
    fb.u = u;
    return fb.f;
}

static std::optional<uint32_t> pixelDataSize(GLsizei width, GLsizei height, GLenum format,
                                             GLenum type, GLint alignment) {
    constexpr uint64_t kMaxReadbackBytes = 256u * 1024u * 1024u;
    if (width <= 0 || height <= 0 ||
        (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) {
        return std::nullopt;
    }

    uint32_t bytesPerPixel = 0;
    switch (type) {
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_5_6_5:
        bytesPerPixel = 2;
        break;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        bytesPerPixel = 4;
        break;
    default: {
        uint32_t componentCount = 0;
        switch (format) {
        case GL_ALPHA:
        case GL_LUMINANCE:
        case GL_RED:
            componentCount = 1;
            break;
        case GL_LUMINANCE_ALPHA:
        case GL_RG:
            componentCount = 2;
            break;
        case GL_RGB:
            componentCount = 3;
            break;
        case GL_RGBA:
        case 0x80E1:
            componentCount = 4;
            break; // GL_BGRA_EXT
        default:
            return std::nullopt;
        }
        uint32_t componentSize = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE:
        case GL_BYTE:
            componentSize = 1;
            break;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
        case GL_HALF_FLOAT:
            componentSize = 2;
            break;
        case GL_UNSIGNED_INT:
        case GL_INT:
        case GL_FLOAT:
            componentSize = 4;
            break;
        default:
            return std::nullopt;
        }
        bytesPerPixel = componentCount * componentSize;
        break;
    }
    }

    const uint64_t row = static_cast<uint64_t>(width) * bytesPerPixel;
    const uint64_t stride =
        (row + static_cast<uint32_t>(alignment) - 1u) & ~(static_cast<uint64_t>(alignment) - 1u);
    const uint64_t size = stride * static_cast<uint64_t>(height - 1) + row;
    if (size > kMaxReadbackBytes || size > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(size);
}

struct ClientAttribute {
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    GuestAddr address = 0;
    bool enabled = false;
    bool clientMemory = false;
    std::vector<uint8_t> data;
};

struct ClientArrayState {
    static constexpr std::size_t kMaxAttributes = 32;
    std::array<ClientAttribute, kMaxAttributes> attributes;
    GLuint arrayBuffer = 0;
    GLuint elementArrayBuffer = 0;
    std::vector<uint8_t> indexData;
};

struct GlStateMirror {
    std::optional<std::array<int32_t, 4>> viewport;
    std::unordered_map<GLenum, GLuint> buffers;
};

static std::optional<uint32_t> glTypeSize(GLenum type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        return 2;
    case GL_FIXED:
    case GL_FLOAT:
    case GL_INT:
    case GL_UNSIGNED_INT:
        return 4;
    default:
        return std::nullopt;
    }
}

static uint32_t maximumIndex(const uint8_t *data, std::size_t count, GLenum type) {
    if (type == GL_UNSIGNED_BYTE) {
        uint32_t maximum = 0;
        std::size_t index = 0;
#if defined(__aarch64__)
        uint8x16_t vectorMaximum = vdupq_n_u8(0);
        for (; index + 16 <= count; index += 16) {
            vectorMaximum = vmaxq_u8(vectorMaximum, vld1q_u8(data + index));
        }
        maximum = vmaxvq_u8(vectorMaximum);
#endif
        for (; index < count; ++index) {
            maximum = std::max<uint32_t>(maximum, data[index]);
        }
        return maximum;
    }

    if (type == GL_UNSIGNED_SHORT) {
        uint32_t maximum = 0;
        std::size_t index = 0;
#if defined(__aarch64__)
        uint16x8_t vectorMaximum = vdupq_n_u16(0);
        for (; index + 8 <= count; index += 8) {
            vectorMaximum = vmaxq_u16(vectorMaximum, vld1q_u16(reinterpret_cast<const uint16_t *>(
                                                         data + index * sizeof(uint16_t))));
        }
        maximum = vmaxvq_u16(vectorMaximum);
#endif
        for (; index < count; ++index) {
            uint16_t value = 0;
            std::memcpy(&value, data + index * sizeof(value), sizeof(value));
            maximum = std::max<uint32_t>(maximum, value);
        }
        return maximum;
    }

    uint32_t maximum = 0;
    std::size_t index = 0;
#if defined(__aarch64__)
    uint32x4_t vectorMaximum = vdupq_n_u32(0);
    for (; index + 4 <= count; index += 4) {
        vectorMaximum = vmaxq_u32(vectorMaximum, vld1q_u32(reinterpret_cast<const uint32_t *>(
                                                     data + index * sizeof(uint32_t))));
    }
    maximum = vmaxvq_u32(vectorMaximum);
#endif
    for (; index < count; ++index) {
        uint32_t value = 0;
        std::memcpy(&value, data + index * sizeof(value), sizeof(value));
        maximum = std::max(maximum, value);
    }
    return maximum;
}

static GuestSize textureParameterValueCount(GLenum pname) {
    constexpr GLenum kTextureBorderColor = 0x1004;
    constexpr GLenum kTextureCropRect = 0x8B9D;
    constexpr GLenum kTextureSwizzleRgba = 0x8E46;
    switch (pname) {
    case kTextureBorderColor:
    case kTextureCropRect:
    case kTextureSwizzleRgba:
        return 4;
    default:
        return 1;
    }
}

static bool prepareClientAttributes(GuestMemory &mem, ClientArrayState &state, uint32_t maxIndex,
                                    uint64_t &directCount, uint64_t &copyCount,
                                    uint64_t &copiedBytes, uint64_t &capacityGrowths) {
    constexpr uint64_t kMaxClientArrayBytes = 64u * 1024u * 1024u;
    for (std::size_t index = 0; index < state.attributes.size(); ++index) {
        auto &attribute = state.attributes[index];
        if (!attribute.enabled || !attribute.clientMemory) {
            continue;
        }
        const auto typeSize = glTypeSize(attribute.type);
        if (!typeSize || attribute.size < 1 || attribute.size > 4 || attribute.stride < 0) {
            LOGE("unsupported client attribute layout at index %zu", index);
            return false;
        }
        const uint64_t elementSize = static_cast<uint64_t>(attribute.size) * *typeSize;
        const uint64_t stride = attribute.stride == 0 ? elementSize : attribute.stride;
        const uint64_t required = static_cast<uint64_t>(maxIndex) * stride + elementSize;
        if (!attribute.address || required == 0 || required > kMaxClientArrayBytes ||
            required > std::numeric_limits<GuestSize>::max()) {
            LOGE("unreadable client attribute at index %zu address=0x%08x bytes=%llu", index,
                 attribute.address, static_cast<unsigned long long>(required));
            return false;
        }
        const auto guestSize = static_cast<GuestSize>(required);
        const uint8_t *data = mem.contiguousData(attribute.address, guestSize);
        if (data) {
            ++directCount;
        } else {
            if (required > attribute.data.capacity()) {
                ++capacityGrowths;
            }
            attribute.data.resize(static_cast<std::size_t>(required));
            if (!mem.copyOut(attribute.address, attribute.data.data(), guestSize)) {
                LOGE("unreadable client attribute at index %zu address=0x%08x bytes=%llu", index,
                     attribute.address, static_cast<unsigned long long>(required));
                return false;
            }
            data = attribute.data.data();
            ++copyCount;
            copiedBytes += required;
        }
        if (state.arrayBuffer != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
        glVertexAttribPointer(static_cast<GLuint>(index), attribute.size, attribute.type,
                              attribute.normalized, attribute.stride, data);
        if (state.arrayBuffer != 0) {
            glBindBuffer(GL_ARRAY_BUFFER, state.arrayBuffer);
        }
    }
    return true;
}

void GlBridge::recordStateCall(StateMetric metric, bool redundant, bool elided) {
    const auto index = static_cast<std::size_t>(metric);
    ++stateCalls_[index];
    if (redundant) {
        ++redundantStateCalls_[index];
    }
    if (elided) {
        ++elidedStateCalls_[index];
    }
}

void GlBridge::recordIssue(std::string issue) {
    ++issueCount_;
    if (firstIssue_.empty()) {
        firstIssue_ = std::move(issue);
    }
}

void GlBridge::captureGlError(const char *operation) {
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) {
        return;
    }
    if (pendingGlError_ == GL_NO_ERROR) {
        pendingGlError_ = error;
    }
    std::ostringstream message;
    message << operation << " error 0x" << std::hex << error;
    recordIssue(message.str());
    LOGE("%s", message.str().c_str());
}

std::string GlBridge::diagnostics() const {
    std::string result;
    if (firstIssue_.empty()) {
        result = "bounded GLES2 profile; no GL failures";
    } else {
        result = "bounded GLES2 profile; " + firstIssue_;
        if (issueCount_ > 1) {
            result += " (" + std::to_string(issueCount_) + " failures)";
        }
    }
    result += "; frames=" + std::to_string(presentedFrames_);
    if (measuredFrameIntervals_ != 0) {
        const double averageFrameMs =
            static_cast<double>(measuredFrameTimeNs_) / measuredFrameIntervals_ / 1'000'000.0;
        const double averageFps = 1'000.0 / averageFrameMs;
        std::ostringstream timing;
        timing.setf(std::ios::fixed);
        timing.precision(2);
        timing << " frame_avg=" << averageFrameMs << "ms (" << averageFps << "fps)"
               << " frame_range=" << static_cast<double>(shortestFrameTimeNs_) / 1'000'000.0 << '-'
               << static_cast<double>(longestFrameTimeNs_) / 1'000'000.0 << "ms";
        if (frameTimingDiscontinuities_ != 0) {
            timing << " discontinuities=" << frameTimingDiscontinuities_;
        }
        result += timing.str();
    }
    if (recentFrameTimeCount_ != 0) {
        uint64_t total = 0;
        uint64_t shortest = std::numeric_limits<uint64_t>::max();
        uint64_t longest = 0;
        for (std::size_t index = 0; index < recentFrameTimeCount_; ++index) {
            total += recentFrameTimesNs_[index];
            shortest = std::min(shortest, recentFrameTimesNs_[index]);
            longest = std::max(longest, recentFrameTimesNs_[index]);
        }
        const double averageMs = static_cast<double>(total) / recentFrameTimeCount_ / 1'000'000.0;
        std::ostringstream timing;
        timing.setf(std::ios::fixed);
        timing.precision(2);
        timing << " recent_frame_avg=" << averageMs << "ms (" << 1'000.0 / averageMs
               << "fps) recent_frame_range=" << static_cast<double>(shortest) / 1'000'000.0 << '-'
               << static_cast<double>(longest) / 1'000'000.0 << "ms";
        result += timing.str();
    }
    if (swapCount_ != 0) {
        std::ostringstream timing;
        timing.setf(std::ios::fixed);
        timing.precision(2);
        timing << " swap_avg=" << static_cast<double>(swapTimeNs_) / swapCount_ / 1'000'000.0
               << "ms swap_max=" << static_cast<double>(longestSwapTimeNs_) / 1'000'000.0 << "ms";
        result += timing.str();
    }
    result += " textures=" + std::to_string(textureUploads_);
    result += " compressed=" + std::to_string(compressedTextureUploads_);
    if (maxTextureWidth_ > 0 && maxTextureHeight_ > 0) {
        result += " max_texture=" + std::to_string(maxTextureWidth_) + "x" +
                  std::to_string(maxTextureHeight_);
    }
    if (defaultViewportWidth_ > 0 && defaultViewportHeight_ > 0) {
        result += " default_viewport=" + std::to_string(defaultViewportWidth_) + "x" +
                  std::to_string(defaultViewportHeight_);
    }
    const auto appendRect = [&result](const char *label, const RectState &rect) {
        if (!rect.valid) {
            return;
        }
        result += " ";
        result += label;
        result += "=" + std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
                  std::to_string(rect.width) + "x" + std::to_string(rect.height);
    };
    appendRect("viewport", lastDefaultViewport_);
    appendRect("offscreen_viewport", lastOffscreenViewport_);
    if (lastScissor_.valid) {
        result += scissorEnabled_ ? " scissor=on:" : " scissor=off:";
        result += std::to_string(lastScissor_.x) + "," + std::to_string(lastScissor_.y) + "," +
                  std::to_string(lastScissor_.width) + "x" + std::to_string(lastScissor_.height);
    }
    result += usedOffscreenFramebuffer_ ? " offscreen_fbo=yes" : " offscreen_fbo=no";
    static constexpr std::array<const char *, static_cast<std::size_t>(StateMetric::Count)>
        stateNames = {"viewport", "bind_buf"};
    uint64_t totalStateCalls = 0;
    uint64_t totalRedundantStateCalls = 0;
    uint64_t totalElidedStateCalls = 0;
    for (std::size_t index = 0; index < stateCalls_.size(); ++index) {
        totalStateCalls += stateCalls_[index];
        totalRedundantStateCalls += redundantStateCalls_[index];
        totalElidedStateCalls += elidedStateCalls_[index];
    }
    result += " state=" + std::to_string(totalStateCalls) + "/" +
              std::to_string(totalRedundantStateCalls) + "/" +
              std::to_string(totalElidedStateCalls);
    result += " client_attr=" + std::to_string(directClientAttributes_) + "/" +
              std::to_string(clientAttributeCopies_) + "/" + std::to_string(clientAttributeBytes_) +
              "B/" + std::to_string(clientAttributeGrowths_);
    result += " client_index=" + std::to_string(directClientIndices_) + "/" +
              std::to_string(clientIndexCopies_) + "/" + std::to_string(clientIndexBytes_) + "B/" +
              std::to_string(clientIndexGrowths_);
    result += " guest_data=" + std::to_string(directGuestData_) + "/" +
              std::to_string(guestDataCopies_) + "/" + std::to_string(guestDataBytes_) + "B/" +
              std::to_string(guestDataGrowths_);
    for (std::size_t index = 0; index < stateCalls_.size(); ++index) {
        if (redundantStateCalls_[index] == 0) {
            continue;
        }
        result += " ";
        result += stateNames[index];
        result += "=" + std::to_string(redundantStateCalls_[index]) + "/" +
                  std::to_string(stateCalls_[index]);
    }
    return result;
}

void GlBridge::recordTextureUpload(int32_t level, int32_t width, int32_t height, bool compressed) {
    if (level != 0 || width <= 0 || height <= 0) {
        return;
    }
    ++textureUploads_;
    if (compressed) {
        ++compressedTextureUploads_;
    }
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
        static_cast<uint64_t>(maxTextureWidth_) * static_cast<uint64_t>(maxTextureHeight_)) {
        maxTextureWidth_ = static_cast<uint32_t>(width);
        maxTextureHeight_ = static_cast<uint32_t>(height);
    }
}

GlBridge::~GlBridge() {
    if (!active_) {
        return;
    }
    if (display_) {
        eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (context_) {
            eglDestroyContext(static_cast<EGLDisplay>(display_), static_cast<EGLContext>(context_));
        }
        if (surface_) {
            eglDestroySurface(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_));
        }
        if (fallbackSurface_) {
            eglDestroySurface(static_cast<EGLDisplay>(display_),
                              static_cast<EGLSurface>(fallbackSurface_));
        }
        eglTerminate(static_cast<EGLDisplay>(display_));
    }
    if (nativeWindow_)
        ANativeWindow_release(static_cast<ANativeWindow *>(nativeWindow_));
    active_ = false;
}

bool GlBridge::init(void *nativeWindow) {
    auto *win = static_cast<ANativeWindow *>(nativeWindow);
    if (!win) {
        return false;
    }

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {EGL_SURFACE_TYPE,
                                    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                    EGL_RENDERABLE_TYPE,
                                    EGL_OPENGL_ES2_BIT,
                                    EGL_RED_SIZE,
                                    8,
                                    EGL_GREEN_SIZE,
                                    8,
                                    EGL_BLUE_SIZE,
                                    8,
                                    EGL_ALPHA_SIZE,
                                    8,
                                    EGL_DEPTH_SIZE,
                                    16,
                                    EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(dpy, configAttribs, &cfg, 1, &numConfigs) || numConfigs < 1) {
        LOGE("eglChooseConfig failed");
        eglTerminate(dpy);
        return false;
    }

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, win, nullptr);
    if (surf == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        eglTerminate(dpy);
        return false;
    }

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return false;
    }

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return false;
    }
    if (!eglSwapInterval(dpy, 1)) {
        LOGE("eglSwapInterval failed: 0x%x", eglGetError());
    }

    display_ = dpy;
    context_ = ctx;
    surface_ = surf;
    config_ = cfg;
    nativeWindow_ = win;
    ANativeWindow_acquire(win);
    active_ = true;
    const auto *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const auto *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    const auto *shadingVersion =
        reinterpret_cast<const char *>(glGetString(GL_SHADING_LANGUAGE_VERSION));
    renderer_ = renderer ? renderer : "unavailable";
    version_ = version ? version : "unavailable";
    LOGI("EGL initialised: renderer=%s, GL=%s, GLSL=%s", renderer_.c_str(), version_.c_str(),
         shadingVersion ? shadingVersion : "unavailable");
    return true;
}

void GlBridge::suspendSurface() {
    if (!active_ || !display_ || !context_)
        return;
    auto display = static_cast<EGLDisplay>(display_);
    auto context = static_cast<EGLContext>(context_);
    if (!fallbackSurface_) {
        const EGLint attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        fallbackSurface_ =
            eglCreatePbufferSurface(display, static_cast<EGLConfig>(config_), attributes);
    }
    const EGLSurface fallback = static_cast<EGLSurface>(fallbackSurface_);
    if (fallback != EGL_NO_SURFACE)
        eglMakeCurrent(display, fallback, fallback, context);
    if (surface_) {
        eglDestroySurface(display, static_cast<EGLSurface>(surface_));
        surface_ = nullptr;
    }
    if (nativeWindow_) {
        ANativeWindow_release(static_cast<ANativeWindow *>(nativeWindow_));
        nativeWindow_ = nullptr;
    }
}

bool GlBridge::attachSurface(void *nativeWindow) {
    auto *win = static_cast<ANativeWindow *>(nativeWindow);
    if (!active_ || !display_ || !context_ || !win)
        return false;
    suspendSurface();
    auto display = static_cast<EGLDisplay>(display_);
    EGLSurface surface =
        eglCreateWindowSurface(display, static_cast<EGLConfig>(config_), win, nullptr);
    if (surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, static_cast<EGLContext>(context_))) {
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        return false;
    }
    if (!eglSwapInterval(display, 1)) {
        LOGE("eglSwapInterval failed after surface attach: 0x%x", eglGetError());
    }
    surface_ = surface;
    nativeWindow_ = win;
    ANativeWindow_acquire(win);
    return true;
}

void GlBridge::swap() {
    if (active_ && surface_) {
        const auto now = std::chrono::steady_clock::now();
        if (lastPresent_ != std::chrono::steady_clock::time_point{}) {
            const auto interval =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastPresent_).count();
            constexpr int64_t kLongestMeasuredFrameNs = 500'000'000;
            if (interval > 0 && interval <= kLongestMeasuredFrameNs) {
                const auto value = static_cast<uint64_t>(interval);
                measuredFrameTimeNs_ += value;
                ++measuredFrameIntervals_;
                shortestFrameTimeNs_ =
                    shortestFrameTimeNs_ == 0 ? value : std::min(shortestFrameTimeNs_, value);
                longestFrameTimeNs_ = std::max(longestFrameTimeNs_, value);
                recentFrameTimesNs_[recentFrameTimeCursor_] = value;
                recentFrameTimeCursor_ = (recentFrameTimeCursor_ + 1) % recentFrameTimesNs_.size();
                recentFrameTimeCount_ =
                    std::min(recentFrameTimeCount_ + 1, recentFrameTimesNs_.size());
            } else {
                ++frameTimingDiscontinuities_;
            }
        }
        lastPresent_ = now;
        ++presentedFrames_;
        const auto swapStart = std::chrono::steady_clock::now();
        if (!eglSwapBuffers(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_))) {
            const EGLint error = eglGetError();
            std::ostringstream message;
            message << "eglSwapBuffers error 0x" << std::hex << error;
            recordIssue(message.str());
            LOGE("%s", message.str().c_str());
        }
        const auto swapDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - swapStart)
                                      .count();
        const auto swapDurationNs = static_cast<uint64_t>(std::max<int64_t>(swapDuration, 0));
        ++swapCount_;
        swapTimeNs_ += swapDurationNs;
        longestSwapTimeNs_ = std::max(longestSwapTimeNs_, swapDurationNs);
    }
}

void GlBridge::registerHandlers(std::unordered_map<std::string, Handler> &handlers,
                                std::unordered_map<GuestAddr, Handler> &extHandlers,
                                GuestMemory &mem) {
    if (!active_) {
        return;
    }
    forwardSimple(handlers, mem);

    handlers["eglGetProcAddress"] = [this, &mem, &handlers, &extHandlers](GuestCpu &cpu) {
        GuestAddr nameAddr = cpu.reg(0);
        if (!nameAddr) {
            cpu.setReg(0, 0);
            return;
        }
        std::string name = mem.readString(nameAddr, 256);
        auto handler = handlers.find(name);
        if (handler == handlers.end()) {
            static const std::unordered_map<std::string, std::string> aliases = {
                {"glBindFramebufferOES", "glBindFramebuffer"},
                {"glBindRenderbufferOES", "glBindRenderbuffer"},
                {"glBlendEquationOES", "glBlendEquation"},
                {"glBlendEquationSeparateOES", "glBlendEquationSeparate"},
                {"glBlendFuncSeparateOES", "glBlendFuncSeparate"},
                {"glCheckFramebufferStatusOES", "glCheckFramebufferStatus"},
                {"glClearDepthfOES", "glClearDepthf"},
                {"glDeleteFramebuffersOES", "glDeleteFramebuffers"},
                {"glDeleteRenderbuffersOES", "glDeleteRenderbuffers"},
                {"glDepthRangefOES", "glDepthRangef"},
                {"glFramebufferRenderbufferOES", "glFramebufferRenderbuffer"},
                {"glFramebufferTexture2DOES", "glFramebufferTexture2D"},
                {"glGenFramebuffersOES", "glGenFramebuffers"},
                {"glGenRenderbuffersOES", "glGenRenderbuffers"},
                {"glGenerateMipmapOES", "glGenerateMipmap"},
                {"glRenderbufferStorageOES", "glRenderbufferStorage"},
            };
            const auto alias = aliases.find(name);
            if (alias != aliases.end()) {
                handler = handlers.find(alias->second);
            }
        }
        if (handler == handlers.end()) {
            cpu.setReg(0, 0);
            return;
        }
        const auto existing = procTraps_.find(name);
        if (existing != procTraps_.end()) {
            cpu.setReg(0, existing->second);
            return;
        }
        if (nextProcTrap_ > kExtTrapEnd - sizeof(uint32_t)) {
            recordIssue("GL procedure trap space exhausted");
            cpu.setReg(0, 0);
            return;
        }
        const GuestAddr trap = nextProcTrap_;
        nextProcTrap_ += sizeof(uint32_t);
        extHandlers[trap] = handler->second;
        procTraps_.emplace(std::move(name), trap);
        cpu.setReg(0, trap);
    };

    handlers["eglSwapBuffers"] = [this](GuestCpu &cpu) {
        swap();
        cpu.setReg(0, 1); // EGL_TRUE
    };
}

void GlBridge::forwardSimple(std::unordered_map<std::string, Handler> &handlers, GuestMemory &mem) {
    auto clientArrays = std::make_shared<ClientArrayState>();
    auto state = std::make_shared<GlStateMirror>();
    auto transferScratch = std::make_shared<std::vector<uint8_t>>();
    const auto readData = [this, &mem, transferScratch](GuestAddr address, GuestSize size) {
        return readGuestData(mem, address, size, *transferScratch, directGuestData_,
                             guestDataCopies_, guestDataBytes_, guestDataGrowths_);
    };

    handlers["glClear"] = [](GuestCpu &cpu) { glClear(cpu.reg(0)); };
    handlers["glEnable"] = [this](GuestCpu &cpu) {
        const GLenum capability = cpu.reg(0);
        if (capability == GL_SCISSOR_TEST) {
            scissorEnabled_ = true;
        }
        glEnable(capability);
    };
    handlers["glDisable"] = [this](GuestCpu &cpu) {
        const GLenum capability = cpu.reg(0);
        if (capability == GL_SCISSOR_TEST) {
            scissorEnabled_ = false;
        }
        glDisable(capability);
    };
    handlers["glDepthFunc"] = [](GuestCpu &cpu) { glDepthFunc(cpu.reg(0)); };
    handlers["glDepthMask"] = [](GuestCpu &cpu) { glDepthMask(cpu.reg(0)); };
    handlers["glCullFace"] = [](GuestCpu &cpu) { glCullFace(cpu.reg(0)); };
    handlers["glFrontFace"] = [](GuestCpu &cpu) { glFrontFace(cpu.reg(0)); };
    handlers["glColorMask"] = [](GuestCpu &cpu) {
        glColorMask(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    };
    handlers["glBlendFunc"] = [](GuestCpu &cpu) { glBlendFunc(cpu.reg(0), cpu.reg(1)); };
    handlers["glBlendEquation"] = [](GuestCpu &cpu) { glBlendEquation(cpu.reg(0)); };
    handlers["glBlendEquationSeparate"] = [](GuestCpu &cpu) {
        glBlendEquationSeparate(cpu.reg(0), cpu.reg(1));
    };
    handlers["glBlendFuncSeparate"] = [](GuestCpu &cpu) {
        glBlendFuncSeparate(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    };
    handlers["glScissor"] = [this](GuestCpu &cpu) {
        lastScissor_ = {
            .x = static_cast<int32_t>(cpu.reg(0)),
            .y = static_cast<int32_t>(cpu.reg(1)),
            .width = static_cast<int32_t>(cpu.reg(2)),
            .height = static_cast<int32_t>(cpu.reg(3)),
            .valid = true,
        };
        glScissor(lastScissor_.x, lastScissor_.y, lastScissor_.width, lastScissor_.height);
    };
    handlers["glViewport"] = [this, state](GuestCpu &cpu) {
        const auto x = static_cast<int32_t>(cpu.reg(0));
        const auto y = static_cast<int32_t>(cpu.reg(1));
        const auto width = static_cast<int32_t>(cpu.reg(2));
        const auto height = static_cast<int32_t>(cpu.reg(3));
        auto &viewport = boundFramebuffer_ == 0 ? lastDefaultViewport_ : lastOffscreenViewport_;
        viewport = {.x = x, .y = y, .width = width, .height = height, .valid = true};
        if (boundFramebuffer_ == 0 && width > 0 && height > 0 &&
            static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
                static_cast<uint64_t>(defaultViewportWidth_) * defaultViewportHeight_) {
            defaultViewportWidth_ = static_cast<uint32_t>(width);
            defaultViewportHeight_ = static_cast<uint32_t>(height);
        }
        const std::array<int32_t, 4> value = {x, y, width, height};
        const bool redundant = state->viewport && *state->viewport == value;
        state->viewport = value;
        recordStateCall(StateMetric::Viewport, redundant, redundant);
        if (redundant) {
            return;
        }
        glViewport(x, y, width, height);
    };
    handlers["glClearStencil"] = [](GuestCpu &cpu) { glClearStencil(cpu.reg(0)); };
    handlers["glStencilFunc"] = [](GuestCpu &cpu) {
        glStencilFunc(cpu.reg(0), cpu.reg(1), cpu.reg(2));
    };
    handlers["glStencilOp"] = [](GuestCpu &cpu) {
        glStencilOp(cpu.reg(0), cpu.reg(1), cpu.reg(2));
    };
    handlers["glStencilMask"] = [](GuestCpu &cpu) { glStencilMask(cpu.reg(0)); };
    handlers["glStencilFuncSeparate"] = [](GuestCpu &cpu) {
        glStencilFuncSeparate(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    };
    handlers["glStencilMaskSeparate"] = [](GuestCpu &cpu) {
        glStencilMaskSeparate(cpu.reg(0), cpu.reg(1));
    };
    handlers["glStencilOpSeparate"] = [](GuestCpu &cpu) {
        glStencilOpSeparate(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    };
    handlers["glHint"] = [](GuestCpu &cpu) { glHint(cpu.reg(0), cpu.reg(1)); };
    handlers["glPixelStorei"] = [](GuestCpu &cpu) { glPixelStorei(cpu.reg(0), cpu.reg(1)); };
    handlers["glActiveTexture"] = [](GuestCpu &cpu) { glActiveTexture(cpu.reg(0)); };
    handlers["glFlush"] = [](GuestCpu &) { glFlush(); };
    handlers["glFinish"] = [](GuestCpu &) { glFinish(); };
    handlers["glDrawArrays"] = [this, &mem, clientArrays](GuestCpu &cpu) {
        const GLint first = static_cast<GLint>(cpu.reg(1));
        const GLsizei count = static_cast<GLsizei>(cpu.reg(2));
        if (first < 0 || count < 0 ||
            (count > 0 &&
             !prepareClientAttributes(mem, *clientArrays, static_cast<uint32_t>(first + count - 1),
                                      directClientAttributes_, clientAttributeCopies_,
                                      clientAttributeBytes_, clientAttributeGrowths_))) {
            return;
        }
        glDrawArrays(cpu.reg(0), first, count);
    };
    handlers["glGetError"] = [this](GuestCpu &cpu) {
        const GLenum error = pendingGlError_ == GL_NO_ERROR
                                 ? glGetError()
                                 : std::exchange(pendingGlError_, GL_NO_ERROR);
        if (error != GL_NO_ERROR) {
            LOGE("glGetError -> 0x%x", error);
        }
        cpu.setReg(0, error);
    };
    handlers["glIsTexture"] = [](GuestCpu &cpu) { cpu.setReg(0, glIsTexture(cpu.reg(0))); };
    handlers["glIsBuffer"] = [](GuestCpu &cpu) { cpu.setReg(0, glIsBuffer(cpu.reg(0))); };
    handlers["glIsFramebuffer"] = [](GuestCpu &cpu) { cpu.setReg(0, glIsFramebuffer(cpu.reg(0))); };
    handlers["glIsRenderbuffer"] = [](GuestCpu &cpu) {
        cpu.setReg(0, glIsRenderbuffer(cpu.reg(0)));
    };
    handlers["glIsProgram"] = [](GuestCpu &cpu) { cpu.setReg(0, glIsProgram(cpu.reg(0))); };
    handlers["glIsShader"] = [](GuestCpu &cpu) { cpu.setReg(0, glIsShader(cpu.reg(0))); };

    handlers["glClearColor"] = [](GuestCpu &cpu) {
        glClearColor(asFloat(cpu.reg(0)), asFloat(cpu.reg(1)), asFloat(cpu.reg(2)),
                     asFloat(cpu.reg(3)));
    };
    handlers["glClearDepthf"] = [](GuestCpu &cpu) { glClearDepthf(asFloat(cpu.reg(0))); };
    handlers["glDepthRangef"] = [](GuestCpu &cpu) {
        glDepthRangef(asFloat(cpu.reg(0)), asFloat(cpu.reg(1)));
    };
    handlers["glLineWidth"] = [](GuestCpu &cpu) { glLineWidth(asFloat(cpu.reg(0))); };
    handlers["glPolygonOffset"] = [](GuestCpu &cpu) {
        glPolygonOffset(asFloat(cpu.reg(0)), asFloat(cpu.reg(1)));
    };
    handlers["glBlendColor"] = [](GuestCpu &cpu) {
        glBlendColor(asFloat(cpu.reg(0)), asFloat(cpu.reg(1)), asFloat(cpu.reg(2)),
                     asFloat(cpu.reg(3)));
    };

    handlers["glBindBuffer"] = [this, clientArrays, state](GuestCpu &cpu) {
        const GLenum target = cpu.reg(0);
        const GLuint buffer = cpu.reg(1);
        bool redundant = false;
        if (target == GL_ARRAY_BUFFER) {
            redundant = clientArrays->arrayBuffer == buffer;
            clientArrays->arrayBuffer = buffer;
        } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
            redundant = clientArrays->elementArrayBuffer == buffer;
            clientArrays->elementArrayBuffer = buffer;
        } else {
            const auto current = state->buffers.find(target);
            redundant = current != state->buffers.end() && current->second == buffer;
            state->buffers[target] = buffer;
        }
        recordStateCall(StateMetric::BindBuffer, redundant, redundant);
        if (redundant) {
            return;
        }
        glBindBuffer(target, buffer);
    };
    handlers["glBindTexture"] = [](GuestCpu &cpu) { glBindTexture(cpu.reg(0), cpu.reg(1)); };
    handlers["glTexParameteri"] = [](GuestCpu &cpu) {
        glTexParameteri(cpu.reg(0), cpu.reg(1), cpu.reg(2));
    };
    handlers["glUseProgram"] = [](GuestCpu &cpu) { glUseProgram(cpu.reg(0)); };
    handlers["glBindFramebuffer"] = [this](GuestCpu &cpu) {
        const GLenum target = cpu.reg(0);
        const GLuint framebuffer = cpu.reg(1);
        boundFramebuffer_ = framebuffer;
        usedOffscreenFramebuffer_ = usedOffscreenFramebuffer_ || boundFramebuffer_ != 0;
        glBindFramebuffer(target, framebuffer);
    };
    handlers["glBindRenderbuffer"] = [](GuestCpu &cpu) {
        glBindRenderbuffer(cpu.reg(0), cpu.reg(1));
    };
    handlers["glBindVertexArray"] = [](GuestCpu &) {};
    handlers["glDisableVertexAttribArray"] = [clientArrays](GuestCpu &cpu) {
        const GLuint index = cpu.reg(0);
        if (index < clientArrays->attributes.size()) {
            clientArrays->attributes[index].enabled = false;
        }
        glDisableVertexAttribArray(index);
    };
    handlers["glEnableVertexAttribArray"] = [clientArrays](GuestCpu &cpu) {
        const GLuint index = cpu.reg(0);
        if (index < clientArrays->attributes.size()) {
            clientArrays->attributes[index].enabled = true;
        }
        glEnableVertexAttribArray(index);
    };

    handlers["glBufferData"] = [readData](GuestCpu &cpu) {
        uint32_t target = cpu.reg(0), size = cpu.reg(1);
        GuestAddr data = cpu.reg(2);
        uint32_t usage = cpu.reg(3);
        if (data && size) {
            const auto *bytes = readData(data, size);
            if (!bytes) {
                return;
            }
            glBufferData(target, size, bytes, usage);
        } else {
            glBufferData(target, size, nullptr, usage);
        }
    };

    handlers["glBufferSubData"] = [readData](GuestCpu &cpu) {
        uint32_t target = cpu.reg(0);
        intptr_t offset = static_cast<intptr_t>(cpu.reg(1));
        uint32_t size = cpu.reg(2);
        GuestAddr data = cpu.reg(3);
        if (data && size) {
            const auto *bytes = readData(data, size);
            if (!bytes) {
                return;
            }
            glBufferSubData(target, offset, size, bytes);
        }
    };

    handlers["glShaderSource"] = [&mem](GuestCpu &cpu) {
        const GLuint shader = cpu.reg(0);
        const GLsizei count = static_cast<GLsizei>(cpu.reg(1));
        const GuestAddr stringArray = cpu.reg(2);
        const GuestAddr lengthArray = cpu.reg(3);
        if (!stringArray || count <= 0 || count > 4096 ||
            !mem.isMapped(stringArray, static_cast<GuestSize>(count) * 4u) ||
            (lengthArray && !mem.isMapped(lengthArray, static_cast<GuestSize>(count) * 4u))) {
            LOGE("glShaderSource rejected argument array");
            return;
        }

        std::vector<std::string> sources;
        std::vector<const char *> ptrs;
        std::vector<GLint> lengths;
        sources.reserve(count);
        ptrs.reserve(count);
        lengths.reserve(count);
        std::size_t totalSize = 0;
        for (GLsizei i = 0; i < count; ++i) {
            const GuestAddr source = mem.read32(stringArray + static_cast<GuestAddr>(i) * 4u);
            const int32_t length =
                lengthArray
                    ? static_cast<int32_t>(mem.read32(lengthArray + static_cast<GuestAddr>(i) * 4u))
                    : -1;
            if (!source) {
                LOGE("glShaderSource rejected null source at index %d", i);
                return;
            }
            if (length >= 0) {
                if (length > 1024 * 1024 || !mem.isMapped(source, static_cast<uint32_t>(length))) {
                    LOGE("glShaderSource rejected source[%d]=0x%08x length=%d", i, source, length);
                    return;
                }
                const auto bytes = mem.read(source, static_cast<uint32_t>(length));
                sources.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            } else {
                sources.push_back(mem.readString(source, 1024 * 1024));
            }
            totalSize += sources.back().size();
            if (totalSize > 4u * 1024u * 1024u) {
                LOGE("glShaderSource rejected total source size %zu", totalSize);
                return;
            }
        }
        for (const auto &source : sources) {
            ptrs.push_back(source.data());
            lengths.push_back(static_cast<GLint>(source.size()));
        }
        glShaderSource(shader, count, ptrs.data(), lengths.data());
    };

    handlers["glReadPixels"] = [&mem](GuestCpu &cpu) {
        const GLint x = static_cast<GLint>(cpu.reg(0));
        const GLint y = static_cast<GLint>(cpu.reg(1));
        const GLsizei width = static_cast<GLsizei>(cpu.reg(2));
        const GLsizei height = static_cast<GLsizei>(cpu.reg(3));
        const GLenum format = cpu.stackArg(0);
        const GLenum type = cpu.stackArg(1);
        const GuestAddr pixels = cpu.stackArg(2);
        GLint alignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        const auto size = pixelDataSize(width, height, format, type, alignment);
        if (!size || !pixels || !mem.isMapped(pixels, *size)) {
            LOGE("glReadPixels rejected output address=0x%08x size=%u", pixels, size.value_or(0));
            return;
        }
        std::vector<uint8_t> output(*size);
        glReadPixels(x, y, width, height, format, type, output.data());
        mem.write(pixels, output.data(), *size);
    };

    handlers["glShaderBinary"] = [](GuestCpu &) {};

    handlers["glProgramBinaryOES"] = [](GuestCpu &) {};

    handlers["glAttachShader"] = [](GuestCpu &cpu) { glAttachShader(cpu.reg(0), cpu.reg(1)); };
    handlers["glDetachShader"] = [](GuestCpu &cpu) { glDetachShader(cpu.reg(0), cpu.reg(1)); };
    handlers["glLinkProgram"] = [this](GuestCpu &cpu) {
        const GLuint program = cpu.reg(0);
        glLinkProgram(program);
        GLint status = GL_FALSE;
        GLint size = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &size);
        if (status == GL_FALSE) {
            std::vector<GLchar> log(static_cast<std::size_t>(std::max(size, 1)));
            glGetProgramInfoLog(program, size, nullptr, log.data());
            const std::string message = "program link failed: " + std::string(log.data());
            recordIssue(message);
            LOGE("glLinkProgram program=%u failed: %s", program, log.data());
        }
    };
    handlers["glCompileShader"] = [this](GuestCpu &cpu) {
        const GLuint shader = cpu.reg(0);
        glCompileShader(shader);
        GLint status = GL_FALSE;
        GLint size = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &size);
        if (status == GL_FALSE) {
            std::vector<GLchar> log(static_cast<std::size_t>(std::max(size, 1)));
            glGetShaderInfoLog(shader, size, nullptr, log.data());
            recordIssue("shader compile failed: " + std::string(log.data()));
            LOGE("glCompileShader shader=%u failed: %s", shader, log.data());
        }
    };
    handlers["glDeleteShader"] = [](GuestCpu &cpu) { glDeleteShader(cpu.reg(0)); };
    handlers["glDeleteProgram"] = [](GuestCpu &cpu) { glDeleteProgram(cpu.reg(0)); };
    handlers["glBindAttribLocation"] = [&mem](GuestCpu &cpu) {
        GLuint prog = cpu.reg(0);
        GLuint idx = cpu.reg(1);
        GuestAddr name = cpu.reg(2);
        if (name) {
            glBindAttribLocation(prog, idx, mem.readString(name, 256).c_str());
        }
    };

    // A pointer is a byte offset while a vertex buffer is bound.
    handlers["glVertexAttribPointer"] = [clientArrays](GuestCpu &cpu) {
        const GLuint index = cpu.reg(0);
        if (index >= clientArrays->attributes.size()) {
            return;
        }
        auto &attribute = clientArrays->attributes[index];
        attribute.size = static_cast<GLint>(cpu.reg(1));
        attribute.type = cpu.reg(2);
        attribute.normalized = static_cast<GLboolean>(cpu.reg(3));
        attribute.stride = static_cast<GLsizei>(cpu.stackArg(0));
        attribute.address = cpu.stackArg(1);
        attribute.clientMemory = clientArrays->arrayBuffer == 0;
        if (attribute.clientMemory) {
            return;
        }
        glVertexAttribPointer(
            index, attribute.size, attribute.type, attribute.normalized, attribute.stride,
            reinterpret_cast<const void *>(static_cast<uintptr_t>(attribute.address)));
    };

    handlers["glDrawElements"] = [this, &mem, clientArrays](GuestCpu &cpu) {
        const GLsizei count = static_cast<GLsizei>(cpu.reg(1));
        const GLenum type = cpu.reg(2);
        const GuestAddr indices = cpu.reg(3);
        if (count < 0) {
            return;
        }
        if (clientArrays->elementArrayBuffer != 0) {
            glDrawElements(cpu.reg(0), count, type,
                           reinterpret_cast<const void *>(static_cast<uintptr_t>(indices)));
            return;
        }
        const auto indexSize = glTypeSize(type);
        if (!indexSize ||
            (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
            LOGE("unsupported client index type 0x%x", type);
            return;
        }
        const uint64_t byteCount = static_cast<uint64_t>(count) * *indexSize;
        if (byteCount > std::numeric_limits<GuestSize>::max() ||
            (byteCount > 0 &&
             (!indices || !mem.isMapped(indices, static_cast<GuestSize>(byteCount))))) {
            LOGE("unreadable client index data address=0x%08x bytes=%llu", indices,
                 static_cast<unsigned long long>(byteCount));
            return;
        }
        const auto guestSize = static_cast<GuestSize>(byteCount);
        const uint8_t *indexPointer =
            byteCount == 0 ? nullptr : mem.contiguousData(indices, guestSize);
        auto &indexData = clientArrays->indexData;
        if (byteCount > 0 && indexPointer) {
            ++directClientIndices_;
        } else if (byteCount > 0) {
            if (byteCount > indexData.capacity()) {
                ++clientIndexGrowths_;
            }
            indexData.resize(static_cast<std::size_t>(byteCount));
            if (!mem.copyOut(indices, indexData.data(), guestSize)) {
                LOGE("unreadable client index data address=0x%08x bytes=%llu", indices,
                     static_cast<unsigned long long>(byteCount));
                return;
            }
            indexPointer = indexData.data();
            ++clientIndexCopies_;
            clientIndexBytes_ += byteCount;
        }
        const uint32_t maxIndex = maximumIndex(indexPointer, static_cast<std::size_t>(count), type);
        if (!prepareClientAttributes(mem, *clientArrays, maxIndex, directClientAttributes_,
                                     clientAttributeCopies_, clientAttributeBytes_,
                                     clientAttributeGrowths_)) {
            return;
        }
        glDrawElements(cpu.reg(0), count, type, indexPointer);
    };

    handlers["glTexImage2D"] = [this, readData](GuestCpu &cpu) {
        GLenum target = cpu.reg(0);
        GLint level = static_cast<GLint>(cpu.reg(1));
        GLint internalformat = static_cast<GLint>(cpu.reg(2));
        GLsizei w = static_cast<GLsizei>(cpu.reg(3));
        GLsizei h = static_cast<GLsizei>(cpu.stackArg(0));
        GLint border = static_cast<GLint>(cpu.stackArg(1));
        GLenum format = cpu.stackArg(2);
        GLenum type = cpu.stackArg(3);
        GuestAddr data = cpu.stackArg(4);
        recordTextureUpload(level, w, h, false);
        if (data) {
            GLint alignment = 4;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            const auto pixelSize = pixelDataSize(w, h, format, type, alignment);
            if (!pixelSize) {
                LOGE("glTexImage2D unsupported pixel layout format=0x%x type=0x%x", format, type);
                return;
            }
            const auto *bytes = readData(data, *pixelSize);
            if (!bytes) {
                LOGE("glTexImage2D guest data is not readable");
                return;
            }
            glTexImage2D(target, level, internalformat, w, h, border, format, type, bytes);
        } else {
            glTexImage2D(target, level, internalformat, w, h, border, format, type, nullptr);
        }
        captureGlError("glTexImage2D");
    };

    handlers["glTexSubImage2D"] = [readData](GuestCpu &cpu) {
        GLenum target = cpu.reg(0);
        GLint level = static_cast<GLint>(cpu.reg(1));
        GLint xoff = static_cast<GLint>(cpu.reg(2));
        GLint yoff = static_cast<GLint>(cpu.reg(3));
        GLsizei w = static_cast<GLsizei>(cpu.stackArg(0));
        GLsizei h = static_cast<GLsizei>(cpu.stackArg(1));
        GLenum format = cpu.stackArg(2);
        GLenum type = cpu.stackArg(3);
        GuestAddr data = cpu.stackArg(4);
        if (data) {
            GLint alignment = 4;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            const auto pixelSize = pixelDataSize(w, h, format, type, alignment);
            if (!pixelSize) {
                LOGE("glTexSubImage2D unsupported pixel layout format=0x%x type=0x%x", format,
                     type);
                return;
            }
            const auto *bytes = readData(data, *pixelSize);
            if (!bytes) {
                LOGE("glTexSubImage2D guest data is not readable");
                return;
            }
            glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, bytes);
        }
    };

    handlers["glCompressedTexImage2D"] = [this, readData](GuestCpu &cpu) {
        GLenum target = cpu.reg(0);
        GLint level = static_cast<GLint>(cpu.reg(1));
        GLenum internalformat = cpu.reg(2);
        GLsizei w = static_cast<GLsizei>(cpu.reg(3));
        GLsizei h = static_cast<GLsizei>(cpu.stackArg(0));
        GLint border = static_cast<GLint>(cpu.stackArg(1));
        GLsizei imageSize = static_cast<GLsizei>(cpu.stackArg(2));
        GuestAddr data = cpu.stackArg(3);
        recordTextureUpload(level, w, h, true);
        if (imageSize < 0) {
            recordIssue("glCompressedTexImage2D received a negative image size");
            return;
        }
        if (data && imageSize > 0) {
            const auto *bytes = readData(data, static_cast<GuestSize>(imageSize));
            if (!bytes) {
                recordIssue("glCompressedTexImage2D guest data is not readable");
                LOGE("glCompressedTexImage2D guest data is not readable");
                return;
            }
            glCompressedTexImage2D(target, level, internalformat, w, h, border, imageSize, bytes);
            captureGlError("glCompressedTexImage2D");
        }
    };

    handlers["glUniform1i"] = [](GuestCpu &cpu) { glUniform1i(cpu.reg(0), cpu.reg(1)); };
    handlers["glUniform1f"] = [](GuestCpu &cpu) { glUniform1f(cpu.reg(0), asFloat(cpu.reg(1))); };
    handlers["glUniform2f"] = [](GuestCpu &cpu) {
        glUniform2f(cpu.reg(0), asFloat(cpu.reg(1)), asFloat(cpu.reg(2)));
    };
    handlers["glUniform3f"] = [](GuestCpu &cpu) {
        glUniform3f(cpu.reg(0), asFloat(cpu.reg(1)), asFloat(cpu.reg(2)), asFloat(cpu.reg(3)));
    };
    handlers["glUniform4f"] = [](GuestCpu &cpu) {
        glUniform4f(cpu.reg(0), asFloat(cpu.reg(1)), asFloat(cpu.reg(2)), asFloat(cpu.reg(3)),
                    asFloat(cpu.stackArg(0)));
    };
    handlers["glUniform1iv"] = [readData](GuestCpu &cpu) {
        GLint loc = cpu.reg(0);
        GLsizei count = cpu.reg(1);
        GuestAddr v = cpu.reg(2);
        if (count > 0 && v) {
            const auto *data = readData(v, static_cast<GuestSize>(count) * sizeof(GLint));
            if (data) {
                glUniform1iv(loc, count, reinterpret_cast<const GLint *>(data));
            }
        }
    };
    handlers["glUniform2iv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data = readData(values, static_cast<GuestSize>(count) * 2u * sizeof(GLint));
            if (data) {
                glUniform2iv(cpu.reg(0), count, reinterpret_cast<const GLint *>(data));
            }
        }
    };
    handlers["glUniform3iv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data = readData(values, static_cast<GuestSize>(count) * 3u * sizeof(GLint));
            if (data) {
                glUniform3iv(cpu.reg(0), count, reinterpret_cast<const GLint *>(data));
            }
        }
    };
    handlers["glUniform4iv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data = readData(values, static_cast<GuestSize>(count) * 4u * sizeof(GLint));
            if (data) {
                glUniform4iv(cpu.reg(0), count, reinterpret_cast<const GLint *>(data));
            }
        }
    };
    handlers["glUniform1fv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data = readData(values, static_cast<GuestSize>(count) * sizeof(GLfloat));
            if (data) {
                glUniform1fv(cpu.reg(0), count, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };
    handlers["glUniform2fv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data =
                readData(values, static_cast<GuestSize>(count) * 2u * sizeof(GLfloat));
            if (data) {
                glUniform2fv(cpu.reg(0), count, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };
    handlers["glUniform3fv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data =
                readData(values, static_cast<GuestSize>(count) * 3u * sizeof(GLfloat));
            if (data) {
                glUniform3fv(cpu.reg(0), count, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };
    handlers["glUniform4fv"] = [readData](GuestCpu &cpu) {
        const GLsizei count = cpu.reg(1);
        const GuestAddr values = cpu.reg(2);
        if (count > 0 && values) {
            const auto *data =
                readData(values, static_cast<GuestSize>(count) * 4u * sizeof(GLfloat));
            if (data) {
                glUniform4fv(cpu.reg(0), count, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };
    handlers["glUniformMatrix4fv"] = [readData](GuestCpu &cpu) {
        GLint loc = cpu.reg(0);
        GLsizei count = cpu.reg(1);
        GLboolean transpose = cpu.reg(2);
        GuestAddr v = cpu.reg(3);
        if (count > 0 && v) {
            const auto *data = readData(v, static_cast<GuestSize>(count) * 64u);
            if (data) {
                glUniformMatrix4fv(loc, count, transpose, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };
    handlers["glUniformMatrix3fv"] = [readData](GuestCpu &cpu) {
        GLint loc = cpu.reg(0);
        GLsizei count = cpu.reg(1);
        GLboolean transpose = cpu.reg(2);
        GuestAddr v = cpu.reg(3);
        if (count > 0 && v) {
            const auto *data = readData(v, static_cast<GuestSize>(count) * 36u);
            if (data) {
                glUniformMatrix3fv(loc, count, transpose, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };
    handlers["glUniformMatrix2fv"] = [readData](GuestCpu &cpu) {
        GLint loc = cpu.reg(0);
        GLsizei count = cpu.reg(1);
        GLboolean transpose = cpu.reg(2);
        GuestAddr v = cpu.reg(3);
        if (count > 0 && v) {
            const auto *data = readData(v, static_cast<GuestSize>(count) * 16u);
            if (data) {
                glUniformMatrix2fv(loc, count, transpose, reinterpret_cast<const GLfloat *>(data));
            }
        }
    };

    handlers["glTexParameterf"] = [](GuestCpu &cpu) {
        glTexParameterf(cpu.reg(0), cpu.reg(1), asFloat(cpu.reg(2)));
    };
    handlers["glTexParameteriv"] = [readData](GuestCpu &cpu) {
        GLenum target = cpu.reg(0);
        GLenum pname = cpu.reg(1);
        GuestAddr params = cpu.reg(2);
        if (params) {
            const auto *data = readData(params, textureParameterValueCount(pname) * sizeof(GLint));
            if (data) {
                glTexParameteriv(target, pname, reinterpret_cast<const GLint *>(data));
            }
        }
    };
    handlers["glFramebufferTexture2D"] = [](GuestCpu &cpu) {
        glFramebufferTexture2D(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3),
                               static_cast<GLint>(cpu.stackArg(0)));
    };
    handlers["glFramebufferRenderbuffer"] = [](GuestCpu &cpu) {
        glFramebufferRenderbuffer(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    };
    handlers["glCheckFramebufferStatus"] = [this](GuestCpu &cpu) {
        const GLenum status = glCheckFramebufferStatus(cpu.reg(0));
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::ostringstream message;
            message << "framebuffer incomplete 0x" << std::hex << status;
            recordIssue(message.str());
            LOGE("%s", message.str().c_str());
        }
        cpu.setReg(0, status);
    };
    handlers["glGetRenderbufferParameteriv"] = [&mem](GuestCpu &cpu) {
        const GuestAddr output = cpu.reg(2);
        if (!output || !mem.isMapped(output, sizeof(GLint))) {
            return;
        }
        GLint value = 0;
        glGetRenderbufferParameteriv(cpu.reg(0), cpu.reg(1), &value);
        mem.write32(output, static_cast<uint32_t>(value));
    };
    handlers["glGetFramebufferAttachmentParameteriv"] = [&mem](GuestCpu &cpu) {
        const GuestAddr output = cpu.reg(3);
        if (!output || !mem.isMapped(output, sizeof(GLint))) {
            return;
        }
        GLint value = 0;
        glGetFramebufferAttachmentParameteriv(cpu.reg(0), cpu.reg(1), cpu.reg(2), &value);
        mem.write32(output, static_cast<uint32_t>(value));
    };
    handlers["glGenerateMipmap"] = [](GuestCpu &cpu) { glGenerateMipmap(cpu.reg(0)); };
    handlers["glRenderbufferStorage"] = [](GuestCpu &cpu) {
        glRenderbufferStorage(cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
    };

    handlers["glCreateShader"] = [](GuestCpu &cpu) {
        GLuint s = glCreateShader(cpu.reg(0));
        cpu.setReg(0, s);
    };
    handlers["glCreateProgram"] = [](GuestCpu &cpu) {
        GLuint p = glCreateProgram();
        cpu.setReg(0, p);
    };
    handlers["glGenTextures"] = [&mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr out = cpu.reg(1);
        if (n > 0 && out) {
            std::vector<GLuint> ids(n);
            glGenTextures(n, ids.data());
            for (GLsizei i = 0; i < n; ++i) {
                mem.write32(out + i * 4, ids[i]);
            }
        }
    };
    handlers["glGenBuffers"] = [&mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr out = cpu.reg(1);
        if (n > 0 && out) {
            std::vector<GLuint> ids(n);
            glGenBuffers(n, ids.data());
            for (GLsizei i = 0; i < n; ++i) {
                mem.write32(out + i * 4, ids[i]);
            }
        }
    };
    handlers["glGenFramebuffers"] = [&mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr out = cpu.reg(1);
        if (n > 0 && out) {
            std::vector<GLuint> ids(n);
            glGenFramebuffers(n, ids.data());
            for (GLsizei i = 0; i < n; ++i) {
                mem.write32(out + i * 4, ids[i]);
            }
        }
    };
    handlers["glGenRenderbuffers"] = [&mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr out = cpu.reg(1);
        if (n > 0 && out) {
            std::vector<GLuint> ids(n);
            glGenRenderbuffers(n, ids.data());
            for (GLsizei i = 0; i < n; ++i) {
                mem.write32(out + i * 4, ids[i]);
            }
        }
    };
    handlers["glDeleteShader"] = [](GuestCpu &cpu) { glDeleteShader(cpu.reg(0)); };
    handlers["glDeleteProgram"] = [](GuestCpu &cpu) { glDeleteProgram(cpu.reg(0)); };
    handlers["glDeleteTextures"] = [&mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr a = cpu.reg(1);
        if (n > 0 && a) {
            std::vector<GLuint> ids(n);
            for (GLsizei i = 0; i < n; ++i) {
                ids[i] = mem.read32(a + i * 4);
            }
            glDeleteTextures(n, ids.data());
        }
    };
    handlers["glDeleteBuffers"] = [&mem, clientArrays, state](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr a = cpu.reg(1);
        if (n > 0 && a) {
            std::vector<GLuint> ids(n);
            for (GLsizei i = 0; i < n; ++i) {
                ids[i] = mem.read32(a + i * 4);
                if (ids[i] == clientArrays->arrayBuffer) {
                    clientArrays->arrayBuffer = 0;
                }
                if (ids[i] == clientArrays->elementArrayBuffer) {
                    clientArrays->elementArrayBuffer = 0;
                }
                for (auto &entry : state->buffers) {
                    if (entry.second == ids[i]) {
                        entry.second = 0;
                    }
                }
            }
            glDeleteBuffers(n, ids.data());
        }
    };
    handlers["glDeleteFramebuffers"] = [this, &mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr a = cpu.reg(1);
        if (n > 0 && a) {
            std::vector<GLuint> ids(n);
            for (GLsizei i = 0; i < n; ++i) {
                ids[i] = mem.read32(a + i * 4);
                if (boundFramebuffer_ == ids[i]) {
                    boundFramebuffer_ = 0;
                }
            }
            glDeleteFramebuffers(n, ids.data());
        }
    };
    handlers["glDeleteRenderbuffers"] = [&mem](GuestCpu &cpu) {
        GLsizei n = cpu.reg(0);
        GuestAddr a = cpu.reg(1);
        if (n > 0 && a) {
            std::vector<GLuint> ids(n);
            for (GLsizei i = 0; i < n; ++i) {
                ids[i] = mem.read32(a + i * 4);
            }
            glDeleteRenderbuffers(n, ids.data());
        }
    };
    handlers["glGetIntegerv"] = [&mem](GuestCpu &cpu) {
        const GLenum name = cpu.reg(0);
        const GuestAddr output = cpu.reg(1);
        if (!output) {
            return;
        }
        if (const auto fixed = glcompat::fixedIntegerQuery(name)) {
            const auto bytes = static_cast<GuestSize>(fixed->count * sizeof(GLint));
            if (!mem.isMapped(output, bytes)) {
                return;
            }
            for (std::size_t index = 0; index < fixed->count; ++index) {
                mem.write32(output + static_cast<GuestAddr>(index * sizeof(GLint)),
                            static_cast<uint32_t>(fixed->values[index]));
            }
            return;
        }
        if (const auto upperBound = glcompat::integerQueryUpperBound(name)) {
            const auto bytes = static_cast<GuestSize>(upperBound->count * sizeof(GLint));
            if (!mem.isMapped(output, bytes)) {
                return;
            }
            std::array<GLint, 4> values{};
            glGetIntegerv(name, values.data());
            for (std::size_t index = 0; index < upperBound->count; ++index) {
                mem.write32(
                    output + static_cast<GuestAddr>(index * sizeof(GLint)),
                    static_cast<uint32_t>(std::min(values[index], upperBound->values[index])));
            }
            return;
        }
        const std::size_t count = glcompat::queryComponentCount(name);
        const auto bytes = static_cast<GuestSize>(count * sizeof(GLint));
        if (count == 0 || count > 4 || !mem.isMapped(output, bytes)) {
            return;
        }
        std::array<GLint, 4> values{};
        glGetIntegerv(name, values.data());
        for (std::size_t index = 0; index < count; ++index) {
            mem.write32(output + static_cast<GuestAddr>(index * sizeof(GLint)),
                        static_cast<uint32_t>(values[index]));
        }
    };
    handlers["glGetBooleanv"] = [&mem](GuestCpu &cpu) {
        const GLenum name = cpu.reg(0);
        const GuestAddr output = cpu.reg(1);
        const std::size_t count = glcompat::queryComponentCount(name);
        if (!output || count == 0 || count > 4 || !mem.isMapped(output, count)) {
            return;
        }
        std::array<GLboolean, 4> values{};
        glGetBooleanv(name, values.data());
        for (std::size_t index = 0; index < count; ++index) {
            mem.write8(output + static_cast<GuestAddr>(index), values[index]);
        }
    };
    handlers["glGetFloatv"] = [&mem](GuestCpu &cpu) {
        const GLenum name = cpu.reg(0);
        const GuestAddr output = cpu.reg(1);
        const std::size_t count = glcompat::queryComponentCount(name);
        const auto bytes = static_cast<GuestSize>(count * sizeof(GLfloat));
        if (!output || count == 0 || count > 4 || !mem.isMapped(output, bytes)) {
            return;
        }
        std::array<GLfloat, 4> values{};
        glGetFloatv(name, values.data());
        for (std::size_t index = 0; index < count; ++index) {
            uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(values[index]));
            std::memcpy(&bits, &values[index], sizeof(bits));
            mem.write32(output + static_cast<GuestAddr>(index * sizeof(GLfloat)), bits);
        }
    };
    handlers["glGetShaderiv"] = [&mem](GuestCpu &cpu) {
        GuestAddr params = cpu.reg(2);
        if (params) {
            GLint v = 0;
            glGetShaderiv(cpu.reg(0), cpu.reg(1), &v);
            mem.write32(params, v);
        }
    };
    handlers["glGetProgramiv"] = [&mem](GuestCpu &cpu) {
        GuestAddr params = cpu.reg(2);
        if (params) {
            GLint v = 0;
            glGetProgramiv(cpu.reg(0), cpu.reg(1), &v);
            mem.write32(params, v);
        }
    };
    auto registerInfoLog = [&handlers, &mem](const char *name, bool shader) {
        handlers[name] = [&mem, shader](GuestCpu &cpu) {
            const GLsizei capacity = static_cast<GLsizei>(cpu.reg(1));
            const GuestAddr lengthOut = cpu.reg(2);
            const GuestAddr logOut = cpu.reg(3);
            if (capacity <= 0 || !logOut ||
                !mem.isMapped(logOut, static_cast<GuestSize>(capacity))) {
                if (lengthOut && mem.isMapped(lengthOut, sizeof(GLsizei))) {
                    mem.write32(lengthOut, 0);
                }
                return;
            }
            std::vector<GLchar> log(static_cast<std::size_t>(capacity));
            GLsizei length = 0;
            if (shader) {
                glGetShaderInfoLog(cpu.reg(0), capacity, &length, log.data());
            } else {
                glGetProgramInfoLog(cpu.reg(0), capacity, &length, log.data());
            }
            const auto copied = std::min<GLsizei>(length + 1, capacity);
            mem.write(logOut, reinterpret_cast<const uint8_t *>(log.data()), copied);
            if (lengthOut && mem.isMapped(lengthOut, sizeof(GLsizei))) {
                mem.write32(lengthOut, static_cast<uint32_t>(length));
            }
        };
    };
    registerInfoLog("glGetShaderInfoLog", true);
    registerInfoLog("glGetProgramInfoLog", false);
    handlers["glGetAttribLocation"] = [&mem](GuestCpu &cpu) {
        GuestAddr nameAddr = cpu.reg(1);
        if (nameAddr) {
            std::string name = mem.readString(nameAddr, 256);
            cpu.setReg(0, glGetAttribLocation(cpu.reg(0), name.c_str()));
        } else {
            cpu.setReg(0, -1);
        }
    };
    handlers["glGetUniformLocation"] = [&mem](GuestCpu &cpu) {
        GuestAddr nameAddr = cpu.reg(1);
        if (nameAddr) {
            std::string name = mem.readString(nameAddr, 256);
            cpu.setReg(0, glGetUniformLocation(cpu.reg(0), name.c_str()));
        } else {
            cpu.setReg(0, -1);
        }
    };
}

} // namespace codboz
