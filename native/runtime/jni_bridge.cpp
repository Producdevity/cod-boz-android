#include <jni.h>

#include "audio/android_audio_backend.hpp"
#include "bridge/dispatcher.hpp"
#include "cpu/dynarmic_cpu.hpp"
#include "guest/guest_address.hpp"
#include "guest/guest_memory.hpp"
#include "loader/file_io.hpp"
#include "loader/importer.hpp"
#include "loader/lzma.hpp"
#include "loader/s3e_image.hpp"
#include "loader/source_profile.hpp"
#include "network/android_nsd_backend.hpp"
#include "runtime.hpp"
#include "s3e/display.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <format>
#include <optional>
#include <string_view>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "codboz", __VA_ARGS__)
#ifndef NDEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "codboz", __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

namespace {
std::mutex gSessionMutex;

enum class SessionState : jint {
    Idle = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Stopped = 4,
    Failed = 5,
};

struct GameSession {
    ~GameSession() {
        requestStop();
        joinWorker();
    }

    void joinWorker() {
        std::lock_guard lock(workerMutex);
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }

    void requestStop() {
        stopRequested.store(true, std::memory_order_release);
        const auto current = state.load(std::memory_order_acquire);
        if (current == SessionState::Starting || current == SessionState::Running) {
            state.store(SessionState::Stopping, std::memory_order_release);
        }
        auto active = runtimeSnapshot();
        if (active) {
            active->requestStop();
        }
    }

    std::shared_ptr<codboz::Runtime> runtimeSnapshot() {
        std::lock_guard lock(runtimeMutex);
        return runtime;
    }

    void setRuntime(std::shared_ptr<codboz::Runtime> value) {
        std::lock_guard lock(runtimeMutex);
        runtime = std::move(value);
    }

    std::atomic<SessionState> state = SessionState::Starting;
    std::atomic<bool> stopRequested = false;
    std::mutex runtimeMutex;
    std::shared_ptr<codboz::Runtime> runtime;
    std::mutex workerMutex;
    std::thread worker;
};

class JavaThreadAttachment {
  public:
    explicit JavaThreadAttachment(JavaVM *vm) : vm_(vm) {
        JNIEnv *env = nullptr;
        if (vm_ && vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED &&
            vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            attached_ = true;
        }
    }

    ~JavaThreadAttachment() {
        if (attached_) {
            vm_->DetachCurrentThread();
        }
    }

    JavaThreadAttachment(const JavaThreadAttachment &) = delete;
    JavaThreadAttachment &operator=(const JavaThreadAttachment &) = delete;

  private:
    JavaVM *vm_ = nullptr;
    bool attached_ = false;
};

std::shared_ptr<GameSession> gSession;

std::shared_ptr<GameSession> activeSession() {
    std::lock_guard lock(gSessionMutex);
    return gSession;
}

std::shared_ptr<codboz::Runtime> activeRuntime() {
    auto session = activeSession();
    return session ? session->runtimeSnapshot() : nullptr;
}

std::string safeReportValue(std::string_view value) {
    constexpr std::size_t kMaximumReportValueLength = 1024;
    std::string result;
    result.reserve(std::min(value.size(), kMaximumReportValueLength));
    for (const char raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character >= 0x20 && character <= 0x7e) {
            result.push_back(raw);
            if (result.size() == kMaximumReportValueLength) {
                break;
            }
        }
    }
    return result;
}

void writeRuntimeReport(const std::string &root, std::string_view status, uint64_t imports = 0,
                        std::string_view fault = {}, std::string_view renderer = {},
                        std::string_view glVersion = {}, std::string_view glDiagnostics = {},
                        codboz::GuestAddr guestPc = 0, uint64_t uptimeMs = 0,
                        std::string_view lastImport = {}, std::string_view runtimeDiagnostics = {},
                        std::string_view voiceDiagnostics = {}) {
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    std::string report = "timestamp_ms=" + std::to_string(timestamp) +
                         "\nstatus=" + std::string(status) +
                         "\nimports=" + std::to_string(imports) + "\n";
    if (!fault.empty()) {
        report += "fault=" + safeReportValue(fault) + "\n";
    }
    if (guestPc != 0) {
        report += std::format("guest_pc=0x{:08x}\n", guestPc);
    }
    if (uptimeMs != 0) {
        report += "uptime_ms=" + std::to_string(uptimeMs) + "\n";
    }
    if (!lastImport.empty()) {
        report += "last_import=" + safeReportValue(lastImport) + "\n";
    }
    if (!renderer.empty()) {
        report += "renderer=" + safeReportValue(renderer) + "\n";
    }
    if (!glVersion.empty()) {
        report += "gl_version=" + safeReportValue(glVersion) + "\n";
    }
    if (!glDiagnostics.empty()) {
        report += "gl_diagnostics=" + safeReportValue(glDiagnostics) + "\n";
    }
    if (!runtimeDiagnostics.empty()) {
        report += "runtime_diagnostics=" + safeReportValue(runtimeDiagnostics) + "\n";
    }
    if (!voiceDiagnostics.empty()) {
        report += "voice_diagnostics=" + safeReportValue(voiceDiagnostics) + "\n";
    }
    const auto path = std::filesystem::path(root).parent_path() / "runtime-report.txt";
    auto temporary = path;
    temporary += ".tmp";
    try {
        codboz::writeFile(temporary.string().c_str(),
                          {reinterpret_cast<const uint8_t *>(report.data()), report.size()});
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            throw std::filesystem::filesystem_error("could not replace runtime report", temporary,
                                                    path, error);
        }
    } catch (const std::exception &e) {
        LOGE("could not write runtime report: %s", e.what());
    }
}

class RuntimeReportWriter {
  public:
    explicit RuntimeReportWriter(std::string root)
        : root_(std::move(root)), worker_([this] { run(); }) {}

    ~RuntimeReportWriter() {
        stop();
    }

    RuntimeReportWriter(const RuntimeReportWriter &) = delete;
    RuntimeReportWriter &operator=(const RuntimeReportWriter &) = delete;

    void submit(const codboz::Runtime::Progress &progress) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            pending_ = progress;
        }
        condition_.notify_one();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

  private:
    void run() {
        for (;;) {
            std::optional<codboz::Runtime::Progress> progress;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (!pending_ && stopping_) {
                    return;
                }
                progress = std::move(pending_);
                pending_.reset();
            }
            writeRuntimeReport(root_, "running", progress->imports, {}, progress->renderer,
                               progress->glVersion, progress->glDiagnostics, progress->pc,
                               progress->uptimeMs, progress->lastImport,
                               progress->runtimeDiagnostics, progress->voiceDiagnostics);
        }
    }

    std::string root_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<codboz::Runtime::Progress> pending_;
    bool stopping_ = false;
    std::thread worker_;
};

std::string jniString(JNIEnv *env, jstring value) {
    if (!value) {
        return {};
    }
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return result;
}

std::vector<std::string> jniStringArray(JNIEnv *env, jobjectArray values) {
    std::vector<std::string> result;
    if (!values) {
        return result;
    }
    const jsize count = env->GetArrayLength(values);
    result.reserve(static_cast<std::size_t>(count));
    for (jsize index = 0; index < count; ++index) {
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
        result.push_back(jniString(env, value));
        if (value) {
            env->DeleteLocalRef(value);
        }
    }
    return result;
}

bool hasWritableExecutableMapping() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        const auto separator = line.find(' ');
        if (separator != std::string::npos && line.size() >= separator + 5 &&
            line.compare(separator + 1, 3, "rwx") == 0) {
            return true;
        }
    }
    return false;
}
} // namespace
#endif

extern "C" JNIEXPORT
    jstring JNICALL Java_dev_producdevity_codboz_NativeRuntime_nativeVersion(JNIEnv *env, jclass) {
    return env->NewStringUTF(codboz::runtimeVersion());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeResourceManifest(JNIEnv *env, jclass) {
    const auto manifest = codboz::resourceManifestJson();
    return env->NewStringUTF(manifest.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_producdevity_codboz_NativeRuntime_nativeImportApk(
    JNIEnv *env, jclass, jstring apkPath, jstring gameDir) {
    const char *apk = env->GetStringUTFChars(apkPath, nullptr);
    const char *dir = env->GetStringUTFChars(gameDir, nullptr);
    codboz::ImportOutcome out;
    if (apk && dir) {
        out = codboz::importApk(apk, dir);
    } else {
        out.error = "out of memory";
    }
    if (apk) {
        env->ReleaseStringUTFChars(apkPath, apk);
    }
    if (dir) {
        env->ReleaseStringUTFChars(gameDir, dir);
    }
    return env->NewStringUTF(codboz::toJson(out).c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeIsImported(JNIEnv *env, jclass, jstring gameDir) {
    const char *dir = env->GetStringUTFChars(gameDir, nullptr);
    jboolean result = (dir && codboz::isImported(dir)) ? JNI_TRUE : JNI_FALSE;
    if (dir) {
        env->ReleaseStringUTFChars(gameDir, dir);
    }
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeIsPrepared(JNIEnv *env, jclass, jstring gameDir) {
    const char *dir = env->GetStringUTFChars(gameDir, nullptr);
    jboolean result = (dir && codboz::isPrepared(dir)) ? JNI_TRUE : JNI_FALSE;
    if (dir) {
        env->ReleaseStringUTFChars(gameDir, dir);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeCpuSelfTest(JNIEnv *env, jclass) {
    std::string out;
    try {
        codboz::GuestMemory mem;
        constexpr codboz::GuestAddr code = 0x1000;
        mem.mapRegion(code, 0x1000);
        mem.mapRegion(0x80000, 0x1000);
        mem.mapRegion(0x2000, 0x2000);
        mem.write32(0x80000, 7);
        mem.write32(0x2000, 0x78563412u);
        mem.write32(0x2fff, 0x89abcdefu);
        mem.write32(code + 0x0, 0xE5910000u);  // ldr r0,[r1]
        mem.write32(code + 0x4, 0xE3A02005u);  // mov r2,#5
        mem.write32(code + 0x8, 0xE0800002u);  // add r0,r0,r2
        mem.write32(code + 0xC, 0xE12FFF1Eu);  // bx lr
        mem.write32(code + 0x20, 0xE5910000u); // ldr r0,[r1]
        mem.write32(code + 0x24, 0xE12FFF1Eu); // bx lr
        mem.write32(code + 0x40, 0xE1910C9Fu); // lda r0,[r1]
        mem.write32(code + 0x44, 0xE12FFF1Eu); // bx lr
        mem.write32(code + 0x60, 0xE5910000u); // ldr r0,[r1]
        mem.write32(code + 0x64, 0xE12FFF1Eu); // bx lr
        codboz::DynarmicCpu cpu(mem, /*handler=*/nullptr);
        cpu.setReg(1, 0x80000);
        cpu.setReg(13, 0x80800);
        cpu.setReg(14, codboz::kCallbackReturnTrap);
        cpu.setThumb(false);
        cpu.setPc(code);
        auto outcome = cpu.run();
        const bool arithmeticOk =
            outcome == codboz::RunOutcome::CallbackReturned && cpu.reg(0) == 12;

        cpu.setReg(1, 0x2fff);
        cpu.setReg(14, codboz::kCallbackReturnTrap);
        cpu.setPc(code + 0x20);
        const bool crossPageOk =
            cpu.run() == codboz::RunOutcome::CallbackReturned && cpu.reg(0) == 0x89abcdefu;

        cpu.setReg(1, 0x2000);
        cpu.setReg(14, codboz::kCallbackReturnTrap);
        cpu.setPc(code + 0x40);
        const bool orderedOk =
            cpu.run() == codboz::RunOutcome::CallbackReturned && cpu.reg(0) == 0x78563412u;

        cpu.setReg(1, 0x90000);
        cpu.setReg(14, codboz::kCallbackReturnTrap);
        cpu.setPc(code + 0x60);
        const bool faultOk =
            cpu.run() == codboz::RunOutcome::Fault && cpu.lastFaultAddr() == 0x90000;

        out = "outcome=" + std::to_string(static_cast<int>(outcome)) +
              " r0=" + std::to_string(arithmeticOk ? 12 : 0) +
              (mem.fastmemPointer() ? " fastmem=1" : " fastmem=0") +
              " cross=" + (crossPageOk ? "1" : "0") + " ordered=" + (orderedOk ? "1" : "0") +
              " fault=" + (faultOk ? "1" : "0");
#ifdef __ANDROID__
        out += hasWritableExecutableMapping() ? " rwx=1" : " rwx=0";
#endif
    } catch (const std::exception &e) {
        out = std::string("exception: ") + e.what();
    }
    return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativePageSize(JNIEnv *, jclass) {
    return static_cast<jint>(sysconf(_SC_PAGESIZE));
}

#ifdef __ANDROID__
extern "C" JNIEXPORT jboolean JNICALL Java_dev_producdevity_codboz_NativeRuntime_nativeBootGame(
    JNIEnv *env, jclass, jstring gameDir, jobject surface, jobject audioBridge, jobject nsdBridge,
    jboolean controllerConnected, jstring deviceModel, jint displayWidth, jint displayHeight) {
    LOGD("nativeBootGame entered");
    const char *dir = env->GetStringUTFChars(gameDir, nullptr);
    std::string root = dir ? dir : "";
    if (dir) {
        env->ReleaseStringUTFChars(gameDir, dir);
    }
    LOGD("gameDir=%s", root.c_str());
    const bool xperiaControls = controllerConnected == JNI_TRUE;
    const std::string model = jniString(env, deviceModel);
    const codboz::GuestDisplay display{static_cast<uint32_t>(displayWidth),
                                       static_cast<uint32_t>(displayHeight)};
    if (!display.isValid()) {
        LOGE("invalid guest display size %dx%d", displayWidth, displayHeight);
        return JNI_FALSE;
    }
    LOGD("input mode=%s model=%s", xperiaControls ? "xperia-controller" : "touchscreen",
         model.c_str());

    if (!codboz::isPrepared(root)) {
        LOGE("game data is not fully prepared");
        return JNI_FALSE;
    }

    {
        std::lock_guard lock(gSessionMutex);
        if (gSession) {
            return JNI_FALSE;
        }
    }

    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    LOGD("ANativeWindow=%p", win);
    if (!win) {
        LOGE("failed to get ANativeWindow");
        return JNI_FALSE;
    }

    std::unique_ptr<codboz::AudioBackend> audio;
    std::unique_ptr<codboz::ZeroConfBackend> zeroConf;
    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        ANativeWindow_release(win);
        return JNI_FALSE;
    }
    try {
        audio = std::make_unique<codboz::AndroidAudioBackend>(env, audioBridge);
        zeroConf = std::make_unique<codboz::AndroidNsdBackend>(env, nsdBridge);
    } catch (const std::exception &e) {
        LOGE("audio bridge unavailable: %s", e.what());
        ANativeWindow_release(win);
        return JNI_FALSE;
    }

    auto session = std::make_shared<GameSession>();
    writeRuntimeReport(root, "starting");
    LOGD("spawning game thread");
    try {
        std::lock_guard lock(gSessionMutex);
        if (gSession) {
            ANativeWindow_release(win);
            return JNI_FALSE;
        }
        gSession = session;
        session->worker = std::thread([root, win, session, xperiaControls, model, display, vm,
                                       audio = std::move(audio),
                                       zeroConf = std::move(zeroConf)]() mutable {
            LOGD("game thread entered");
            JavaThreadAttachment attachment(vm);
            bool failed = false;
            RuntimeReportWriter reportWriter(root);
            try {
                LOGD("reading boz.s3e");
                std::string s3ePath = root + "/assets/boz.s3e";
                auto raw = codboz::readFile(s3ePath.c_str());
                LOGD("boz.s3e read (%zu bytes), decompressing", raw.size());
                auto inflated =
                    codboz::lzmaDecompress({raw.data(), static_cast<uint32_t>(raw.size())});
                LOGD("decompressed to %zu bytes", inflated.size());
                codboz::S3eImage image({inflated.data(), static_cast<uint32_t>(inflated.size())});
                LOGD("S3E image parsed (%u symbols, %u relocs)",
                     static_cast<unsigned>(image.symbols().size()),
                     static_cast<unsigned>(image.externalRelocs().size()));

                codboz::GuestMemory mem;
                codboz::Runtime::Config cfg;
                cfg.root = root;
                cfg.saveRoot = (std::filesystem::path(root).parent_path() / "saves").string();
                cfg.controlConfigPath =
                    (std::filesystem::path(root).parent_path() / "config" / "config.txt").string();
#ifndef NDEBUG
                cfg.debugLog = true;
#endif
                cfg.xperiaControls = xperiaControls;
                cfg.deviceModel = model.empty() ? "Android" : model;
                cfg.display = display;
                cfg.progressCallback = [session,
                                        &reportWriter](const codboz::Runtime::Progress &progress) {
                    session->state.store(SessionState::Running, std::memory_order_release);
                    reportWriter.submit(progress);
                };

                LOGD("creating Runtime");
                auto runtime = std::make_shared<codboz::Runtime>(
                    mem, image.symbols(), cfg, std::move(audio), std::move(zeroConf));
                session->setRuntime(runtime);
                if (session->stopRequested.load(std::memory_order_acquire)) {
                    runtime->requestStop();
                    reportWriter.stop();
                } else {
                    LOGD("attaching initial surface");
                    if (!runtime->setSurface(win)) {
                        throw std::runtime_error("could not initialize EGL surface");
                    }
                    ANativeWindow_release(win);
                    win = nullptr;
                    LOGD("guest boot started");
                    auto result = runtime->boot(image);
                    failed = result.outcome == codboz::RunOutcome::Fault;
                    if (failed) {
                        LOGE("guest faulted after %llu imports: %s",
                             static_cast<unsigned long long>(result.importsHandled),
                             result.faultDetail.c_str());
                    } else {
                        LOGD("guest stopped outcome=%d imports=%llu",
                             static_cast<int>(result.outcome),
                             static_cast<unsigned long long>(result.importsHandled));
                    }
                    reportWriter.stop();
                    writeRuntimeReport(root, failed ? "fault" : "stopped", result.importsHandled,
                                       result.faultDetail, runtime->hostRenderer(),
                                       runtime->hostGlVersion(), runtime->hostGlDiagnostics(), 0, 0,
                                       result.lastImport, runtime->hostRuntimeDiagnostics(),
                                       runtime->hostVoiceDiagnostics());
                }
            } catch (const std::exception &e) {
                failed = true;
                LOGE("game thread exception: %s", e.what());
                reportWriter.stop();
                writeRuntimeReport(root, "exception", 0, "runtime initialization failed");
            }
            session->setRuntime(nullptr);
            session->state.store(failed ? SessionState::Failed : SessionState::Stopped,
                                 std::memory_order_release);
            if (win) {
                ANativeWindow_release(win);
            }
            LOGD("game thread exiting");
        });
    } catch (const std::exception &e) {
        LOGE("could not start game thread: %s", e.what());
        if (gSession == session) {
            gSession.reset();
        }
        ANativeWindow_release(win);
        return JNI_FALSE;
    }
    LOGD("nativeBootGame returning");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeStopGame(JNIEnv *, jclass) {
    auto session = activeSession();
    if (session) {
        session->requestStop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeWaitForStopped(JNIEnv *, jclass) {
    auto session = activeSession();
    if (!session) {
        return;
    }
    session->requestStop();
    session->joinWorker();
    std::lock_guard lock(gSessionMutex);
    if (gSession == session) {
        gSession.reset();
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeRuntimeState(JNIEnv *, jclass) {
    auto session = activeSession();
    return session ? static_cast<jint>(session->state.load(std::memory_order_acquire))
                   : static_cast<jint>(SessionState::Idle);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeSetPaused(JNIEnv *, jclass, jboolean paused) {
    auto runtime = activeRuntime();
    if (runtime)
        runtime->setPaused(paused == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeAttachSurface(JNIEnv *env, jclass,
                                                               jobject surface) {
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return JNI_FALSE;
    auto runtime = activeRuntime();
    const bool attached = runtime && runtime->attachSurface(window);
    ANativeWindow_release(window);
    return attached ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_dev_producdevity_codboz_NativeRuntime_nativePointerEvent(
    JNIEnv *, jclass, jint action, jint touchId, jint x, jint y) {
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->queuePointerEvent(action, touchId, x, y);
    }
}

extern "C" JNIEXPORT void JNICALL Java_dev_producdevity_codboz_NativeRuntime_nativeControllerButton(
    JNIEnv *, jclass, jint action, jboolean pressed) {
    auto runtime = activeRuntime();
    if (runtime && action >= 0 && action < static_cast<jint>(codboz::ControllerAction::Count)) {
        runtime->queueControllerButton(static_cast<codboz::ControllerAction>(action),
                                       pressed == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL Java_dev_producdevity_codboz_NativeRuntime_nativeControllerAxes(
    JNIEnv *, jclass, jfloat leftX, jfloat leftY, jfloat rightX, jfloat rightY) {
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->queueControllerAxes(leftX, leftY, rightX, rightY);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeControllerDisconnected(JNIEnv *, jclass) {
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->releaseController();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_producdevity_codboz_NativeRuntime_nativeSetControllerConnected(JNIEnv *, jclass,
                                                                        jboolean connected) {
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->setControllerConnected(connected == JNI_TRUE);
    }
}

extern "C" JNIEXPORT void JNICALL Java_dev_producdevity_codboz_AndroidNsdBridge_nativeServiceFound(
    JNIEnv *env, jobject, jint searchHandle, jstring serviceKey, jstring name, jstring serviceType,
    jstring domain, jstring host, jint port, jbyteArray ipv4Address, jobjectArray txtRecords) {
    if (!ipv4Address || env->GetArrayLength(ipv4Address) != 4 || port < 1 || port > 65535) {
        return;
    }
    uint32_t ipv4 = 0;
    env->GetByteArrayRegion(ipv4Address, 0, 4, reinterpret_cast<jbyte *>(&ipv4));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return;
    }
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->queueZeroConfFound(
            static_cast<uint32_t>(searchHandle), jniString(env, serviceKey), jniString(env, name),
            jniString(env, serviceType), jniString(env, domain), jniString(env, host),
            static_cast<uint16_t>(port), ipv4, jniStringArray(env, txtRecords));
    }
}

extern "C" JNIEXPORT void JNICALL Java_dev_producdevity_codboz_AndroidNsdBridge_nativeServiceLost(
    JNIEnv *env, jobject, jint searchHandle, jstring serviceKey) {
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->queueZeroConfLost(static_cast<uint32_t>(searchHandle), jniString(env, serviceKey));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_producdevity_codboz_AndroidNsdBridge_nativeOperationFailed(JNIEnv *, jobject, jint handle,
                                                                    jboolean publication) {
    auto runtime = activeRuntime();
    if (runtime) {
        runtime->queueZeroConfFailure(static_cast<uint32_t>(handle), publication == JNI_TRUE);
    }
}
#endif
