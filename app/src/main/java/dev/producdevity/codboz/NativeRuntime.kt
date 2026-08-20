package dev.producdevity.codboz

object NativeRuntime {
    const val STATE_IDLE = 0
    const val STATE_STARTING = 1
    const val STATE_RUNNING = 2
    const val STATE_STOPPING = 3
    const val STATE_STOPPED = 4
    const val STATE_FAILED = 5

    init {
        System.loadLibrary("codboz_runtime")
    }

    external fun nativeVersion(): String

    external fun nativeResourceManifest(): String

    external fun nativeImportApk(
        apkPath: String,
        gameDir: String,
    ): String

    external fun nativeIsImported(gameDir: String): Boolean

    external fun nativeIsPrepared(gameDir: String): Boolean

    external fun nativeCpuSelfTest(): String

    external fun nativePageSize(): Int

    external fun nativeBootGame(
        gameDir: String,
        surface: android.view.Surface,
        audioBridge: AndroidAudioBridge,
        nsdBridge: AndroidNsdBridge,
        controllerConnected: Boolean,
        deviceModel: String,
        displayWidth: Int,
        displayHeight: Int,
    ): Boolean

    external fun nativeStopGame()

    external fun nativeWaitForStopped()

    external fun nativeRuntimeState(): Int

    external fun nativeSetPaused(paused: Boolean)

    external fun nativeAttachSurface(surface: android.view.Surface): Boolean

    external fun nativePointerEvent(
        action: Int,
        touchId: Int,
        x: Int,
        y: Int,
    )

    external fun nativeControllerButton(
        action: Int,
        pressed: Boolean,
    )

    external fun nativeControllerAxes(
        leftX: Float,
        leftY: Float,
        rightX: Float,
        rightY: Float,
    )

    external fun nativeControllerDisconnected()

    external fun nativeSetControllerConnected(connected: Boolean)
}
