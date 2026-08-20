package dev.producdevity.codboz

import android.annotation.SuppressLint
import android.app.Activity
import android.graphics.Color
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.Toast
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import java.io.File

class GameActivity :
    Activity(),
    SurfaceHolder.Callback,
    InputManager.InputDeviceListener {
    companion object {
        const val EXTRA_GAME_DIR = "dev.producdevity.codboz.GAME_DIR"
        private const val TARGET_FRAME_RATE = 60f
        private const val MAX_INPUT_DIAGNOSTIC_EVENTS = 24
        private const val RUNTIME_POLL_INTERVAL_MS = 100L
        private const val BACK_EXIT_CONFIRMATION_MS = 2_000L
    }

    private lateinit var gameDir: String
    private lateinit var audioBridge: AndroidAudioBridge
    private lateinit var nsdBridge: AndroidNsdBridge
    private lateinit var guestDisplay: SurfaceSize
    private val mainHandler = Handler(Looper.getMainLooper())
    private var booted = false
    private var bootRequested = false
    private var destroyed = false
    private var resumed = false
    private var activeSurface: Surface? = null
    private lateinit var inputManager: InputManager
    private val digitalControllerState = BooleanArray(ControllerAction.entries.size)
    private val dispatchedControllerState = BooleanArray(ControllerAction.entries.size)
    private var leftTriggerPressed = false
    private var rightTriggerPressed = false
    private val hatPressed = BooleanArray(4)
    private val controllerDeviceIds = mutableSetOf<Int>()
    private val activeTouches = mutableMapOf<Int, GuestPointerPosition>()
    private val movementRecorded = mutableSetOf<Int>()
    private var inputDiagnosticEvents = 0
    private var controllerMotionRecorded = false
    private var lastBackPressedAt = 0L
    private val backCallback = OnBackInvokedCallback(::handleBackPressed)
    private val runtimePoll =
        object : Runnable {
            override fun run() {
                if (destroyed) return
                when (NativeRuntime.nativeRuntimeState()) {
                    NativeRuntime.STATE_IDLE -> {
                        if (!bootRequested) activeSurface?.takeIf(Surface::isValid)?.let(::bootGame)
                    }

                    NativeRuntime.STATE_STARTING -> {
                        CrashDiagnostics.setProcessState(this@GameActivity, "game-starting")
                    }

                    NativeRuntime.STATE_RUNNING -> {
                        if (!booted) {
                            booted = true
                            val mode = if (controllerDeviceIds.isEmpty()) "touch" else "controller"
                            CrashDiagnostics.setProcessState(this@GameActivity, "game-running-$mode")
                        }
                    }

                    NativeRuntime.STATE_STOPPING -> {
                        CrashDiagnostics.setProcessState(this@GameActivity, "game-stopping")
                    }

                    NativeRuntime.STATE_STOPPED -> {
                        booted = false
                        CrashDiagnostics.setProcessState(this@GameActivity, "game-stopped")
                    }

                    NativeRuntime.STATE_FAILED -> {
                        booted = false
                        if (bootRequested) {
                            CrashDiagnostics.setProcessState(this@GameActivity, "game-failed")
                            return
                        }
                    }
                }
                mainHandler.postDelayed(this, RUNTIME_POLL_INTERVAL_MS)
            }
        }

    private enum class ControllerAction(
        val nativeValue: Int,
    ) {
        ACCEPT(0),
        BACK(1),
        SPRINT(2),
        RELOAD(3),
        CHANGE_WEAPON(4),
        TACTICAL_GRENADE(5),
        GRENADE(6),
        AIM(7),
        FIRE(8),
        MELEE(9),
        PAUSE(10),
        DPAD_UP(11),
        DPAD_DOWN(12),
        DPAD_LEFT(13),
        DPAD_RIGHT(14),
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        gameDir = intent.getStringExtra(EXTRA_GAME_DIR) ?: run {
            finish()
            return
        }
        audioBridge = AndroidAudioBridge(applicationContext)
        nsdBridge = AndroidNsdBridge(applicationContext)
        inputManager = getSystemService(InputManager::class.java)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            @Suppress("DEPRECATION")
            window.setDecorFitsSystemWindows(false)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                backCallback,
            )
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes =
                window.attributes.apply {
                    layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
        }
        val displayMetrics = resources.displayMetrics
        guestDisplay =
            DisplayGeometry.guestSurface(displayMetrics.widthPixels, displayMetrics.heightPixels)
        val guestWidth = guestDisplay.width
        val guestHeight = guestDisplay.height
        val sv = SurfaceView(this)
        sv.holder.setFixedSize(guestWidth, guestHeight)
        sv.holder.addCallback(this)
        sv.setOnTouchListener { view, event ->
            fun send(
                pointerIndex: Int,
                action: Int,
                x: Float = event.getX(pointerIndex),
                y: Float = event.getY(pointerIndex),
                requireInsideContent: Boolean = false,
            ): GuestPointerPosition? {
                val position =
                    DisplayGeometry.mapPointer(
                        x,
                        y,
                        view.width.coerceAtLeast(1),
                        view.height.coerceAtLeast(1),
                        guestWidth,
                        guestHeight,
                    )
                if (requireInsideContent && !position.insideContent) return null
                NativeRuntime.nativePointerEvent(
                    action,
                    event.getPointerId(pointerIndex),
                    position.x,
                    position.y,
                )
                return position
            }

            fun sendMove(
                pointerIndex: Int,
                x: Float,
                y: Float,
            ) {
                val pointerId = event.getPointerId(pointerIndex)
                val previous = activeTouches[pointerId] ?: return
                send(pointerIndex, 2, x, y)?.let { position ->
                    activeTouches[pointerId] = position
                    if (position != previous && movementRecorded.add(pointerId)) {
                        recordInputEvent(
                            "touch-move id=$pointerId guest=${previous.x},${previous.y}->" +
                                "${position.x},${position.y}",
                        )
                    }
                }
            }

            fun sendMoves() {
                for (pointerIndex in 0 until event.pointerCount) {
                    sendMove(pointerIndex, event.getX(pointerIndex), event.getY(pointerIndex))
                }
            }

            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN,
                MotionEvent.ACTION_POINTER_DOWN,
                -> {
                    val pointerId = event.getPointerId(event.actionIndex)
                    val position = send(event.actionIndex, 0, requireInsideContent = true)
                    if (position != null) {
                        activeTouches[pointerId] = position
                        movementRecorded -= pointerId
                        recordInputEvent(
                            "touch-down id=$pointerId raw=${event.getX(event.actionIndex).toInt()}," +
                                "${event.getY(event.actionIndex).toInt()} view=${view.width}x${view.height} " +
                                "guest=${position.x},${position.y} source=0x${event.source.toString(16)}",
                        )
                    }
                }

                MotionEvent.ACTION_MOVE -> {
                    sendMoves()
                }

                MotionEvent.ACTION_UP,
                MotionEvent.ACTION_POINTER_UP,
                -> {
                    val pointerId = event.getPointerId(event.actionIndex)
                    if (pointerId in activeTouches) {
                        send(event.actionIndex, 1)?.let { activeTouches[pointerId] = it }
                        activeTouches -= pointerId
                        movementRecorded -= pointerId
                        if (event.actionMasked == MotionEvent.ACTION_UP) view.performClick()
                    }
                }

                MotionEvent.ACTION_CANCEL -> {
                    for (index in 0 until event.pointerCount) {
                        val pointerId = event.getPointerId(index)
                        if (pointerId in activeTouches) send(index, 3)
                    }
                    activeTouches.clear()
                    movementRecorded.clear()
                }
            }
            true
        }
        val container = FrameLayout(this)
        container.setBackgroundColor(Color.BLACK)
        val gameFrame = AspectRatioFrameLayout(this, guestWidth, guestHeight)
        gameFrame.addView(
            sv,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ),
        )
        container.addView(
            gameFrame,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER,
            ),
        )
        setContentView(container)
        hideSystemBars()
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.windowInsetsController?.apply {
                hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility =
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemBars()
        } else {
            cancelActiveTouches()
        }
    }

    override fun onStart() {
        super.onStart()
        inputManager.registerInputDeviceListener(this, null)
        refreshControllerDevices()
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        CrashDiagnostics.setProcessState(this, if (booted) "game-running" else "game-starting")
        resumed = true
        if (bootRequested) NativeRuntime.nativeSetPaused(false)
        audioBridge.onHostResume()
    }

    override fun onPause() {
        CrashDiagnostics.setProcessState(this, "game-paused")
        resumed = false
        cancelActiveTouches()
        releaseController()
        NativeRuntime.nativeSetPaused(true)
        audioBridge.onHostPause()
        super.onPause()
    }

    override fun onStop() {
        inputManager.unregisterInputDeviceListener(this)
        super.onStop()
    }

    override fun onDestroy() {
        destroyed = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.unregisterOnBackInvokedCallback(backCallback)
        }
        mainHandler.removeCallbacks(runtimePoll)
        NativeRuntime.nativeStopGame()
        Thread(
            {
                NativeRuntime.nativeWaitForStopped()
                audioBridge.shutdown()
            },
            "codboz-runtime-shutdown",
        ).start()
        super.onDestroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        activeSurface = holder.surface
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            holder.surface.setFrameRate(
                TARGET_FRAME_RATE,
                Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,
            )
        }
        if (!bootRequested) {
            bootGame(holder.surface)
        } else {
            NativeRuntime.nativeAttachSurface(holder.surface)
            if (resumed) NativeRuntime.nativeSetPaused(false)
        }
    }

    override fun surfaceChanged(
        holder: SurfaceHolder,
        format: Int,
        width: Int,
        height: Int,
    ) {
        Unit
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        activeSurface = null
        cancelActiveTouches()
        NativeRuntime.nativeSetPaused(true)
    }

    private fun bootGame(surface: Surface) {
        if (destroyed || bootRequested || !surface.isValid) return
        if (NativeRuntime.nativeRuntimeState() != NativeRuntime.STATE_IDLE) {
            mainHandler.removeCallbacks(runtimePoll)
            mainHandler.post(runtimePoll)
            return
        }
        val controllerConnected = refreshControllerDevices().isNotEmpty()
        val voiceConfigured =
            ConfigStore(File(filesDir, "config")).read()["voice_chat_enabled"] == "1"
        CrashDiagnostics.recordEvent(
            this,
            "voice-chat configured=$voiceConfigured available=${audioBridge.isVoiceAvailable()}",
        )
        CrashDiagnostics.setProcessState(
            this,
            if (controllerConnected) "game-starting-controller" else "game-starting-touch",
        )
        bootRequested =
            NativeRuntime.nativeBootGame(
                gameDir,
                surface,
                audioBridge,
                nsdBridge,
                controllerConnected,
                Build.MODEL.ifBlank { "Android" },
                guestDisplay.width,
                guestDisplay.height,
            )
        if (!bootRequested) {
            CrashDiagnostics.setProcessState(this, "game-start-failed")
            return
        }
        mainHandler.removeCallbacks(runtimePoll)
        mainHandler.post(runtimePoll)
    }

    private fun cancelActiveTouches() {
        activeTouches.forEach { (pointerId, position) ->
            NativeRuntime.nativePointerEvent(3, pointerId, position.x, position.y)
        }
        activeTouches.clear()
        movementRecorded.clear()
    }

    @SuppressLint("GestureBackNavigation")
    @Deprecated("Handled through OnBackInvokedDispatcher on Android 13 and later")
    override fun onBackPressed() {
        handleBackPressed()
    }

    private fun handleBackPressed() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastBackPressedAt <= BACK_EXIT_CONFIRMATION_MS) {
            finish()
            return
        }
        lastBackPressedAt = now
        Toast.makeText(this, R.string.press_back_again_to_exit, Toast.LENGTH_SHORT).show()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (!isGameControllerSource(event.source)) {
            return super.dispatchKeyEvent(event)
        }
        if (!event.isControllerEvent()) {
            recordInputEvent(
                "ignored-controller-key id=${event.deviceId} source=0x${event.source.toString(16)} " +
                    "key=${event.keyCode}",
            )
            return super.dispatchKeyEvent(event)
        }
        val action =
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A -> ControllerAction.ACCEPT
                KeyEvent.KEYCODE_BUTTON_B -> ControllerAction.BACK
                KeyEvent.KEYCODE_BUTTON_X -> ControllerAction.RELOAD
                KeyEvent.KEYCODE_BUTTON_Y -> ControllerAction.CHANGE_WEAPON
                KeyEvent.KEYCODE_BUTTON_L1 -> ControllerAction.TACTICAL_GRENADE
                KeyEvent.KEYCODE_BUTTON_R1 -> ControllerAction.GRENADE
                KeyEvent.KEYCODE_BUTTON_L2 -> ControllerAction.AIM
                KeyEvent.KEYCODE_BUTTON_R2 -> ControllerAction.FIRE
                KeyEvent.KEYCODE_BUTTON_THUMBL -> ControllerAction.SPRINT
                KeyEvent.KEYCODE_BUTTON_THUMBR -> ControllerAction.MELEE
                KeyEvent.KEYCODE_BUTTON_START -> ControllerAction.PAUSE
                KeyEvent.KEYCODE_DPAD_UP -> ControllerAction.DPAD_UP
                KeyEvent.KEYCODE_DPAD_DOWN -> ControllerAction.DPAD_DOWN
                KeyEvent.KEYCODE_DPAD_LEFT -> ControllerAction.DPAD_LEFT
                KeyEvent.KEYCODE_DPAD_RIGHT -> ControllerAction.DPAD_RIGHT
                else -> null
            } ?: return super.dispatchKeyEvent(event)
        if (event.repeatCount == 0) {
            recordInputEvent(
                "controller-key id=${event.deviceId} key=${event.keyCode} down=${event.action == KeyEvent.ACTION_DOWN}",
            )
            digitalControllerState[action.ordinal] = event.action == KeyEvent.ACTION_DOWN
            updateControllerAction(action)
        }
        return true
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.action != MotionEvent.ACTION_MOVE ||
            event.source and InputDevice.SOURCE_CLASS_JOYSTICK == 0
        ) {
            return super.dispatchGenericMotionEvent(event)
        }
        val device = event.device ?: return super.dispatchGenericMotionEvent(event)
        if (!device.isPhysicalExternalController()) {
            recordInputEvent(
                "ignored-controller-motion id=${event.deviceId} " +
                    "source=0x${event.source.toString(16)}",
            )
            return super.dispatchGenericMotionEvent(event)
        }
        val leftX = centeredAxis(event, device, MotionEvent.AXIS_X)
        val leftY = centeredAxis(event, device, MotionEvent.AXIS_Y)
        val (rightX, rightY) =
            centeredAxisPair(
                event,
                device,
                MotionEvent.AXIS_Z,
                MotionEvent.AXIS_RZ,
                MotionEvent.AXIS_RX,
                MotionEvent.AXIS_RY,
            )
        if (!controllerMotionRecorded) {
            controllerMotionRecorded = true
            recordInputEvent(
                "controller-motion id=${event.deviceId} axes=" +
                    listOf(leftX, leftY, rightX, rightY).joinToString(",") {
                        (it * 1000f).toInt().toString()
                    },
            )
        }
        NativeRuntime.nativeControllerAxes(leftX, leftY, rightX, rightY)
        updateTrigger(
            ControllerAction.AIM,
            triggerAxis(event, device, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE),
            true,
        )
        updateTrigger(
            ControllerAction.FIRE,
            triggerAxis(event, device, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS),
            false,
        )
        updateDpad(ControllerAction.DPAD_UP, event.getAxisValue(MotionEvent.AXIS_HAT_Y) < -0.5f, 0)
        updateDpad(ControllerAction.DPAD_DOWN, event.getAxisValue(MotionEvent.AXIS_HAT_Y) > 0.5f, 1)
        updateDpad(ControllerAction.DPAD_LEFT, event.getAxisValue(MotionEvent.AXIS_HAT_X) < -0.5f, 2)
        updateDpad(ControllerAction.DPAD_RIGHT, event.getAxisValue(MotionEvent.AXIS_HAT_X) > 0.5f, 3)
        return true
    }

    private fun centeredAxis(
        event: MotionEvent,
        device: InputDevice,
        primaryAxis: Int,
    ): Float {
        val range = joystickRange(device, primaryAxis, event.source) ?: return 0f
        val value = event.getAxisValue(range.axis)
        return normalizeCenteredAxis(value, range.min, range.max, range.flat)
    }

    private fun centeredAxisPair(
        event: MotionEvent,
        device: InputDevice,
        primaryX: Int,
        primaryY: Int,
        fallbackX: Int,
        fallbackY: Int,
    ): Pair<Float, Float> {
        val primaryXRange = joystickRange(device, primaryX, event.source)
        val primaryYRange = joystickRange(device, primaryY, event.source)
        val (xRange, yRange) =
            if (primaryXRange != null && primaryYRange != null) {
                primaryXRange to primaryYRange
            } else {
                val fallbackXRange = joystickRange(device, fallbackX, event.source)
                val fallbackYRange = joystickRange(device, fallbackY, event.source)
                if (fallbackXRange == null || fallbackYRange == null) return 0f to 0f
                fallbackXRange to fallbackYRange
            }
        return normalizeCenteredAxis(
            event.getAxisValue(xRange.axis),
            xRange.min,
            xRange.max,
            xRange.flat,
        ) to
            normalizeCenteredAxis(
                event.getAxisValue(yRange.axis),
                yRange.min,
                yRange.max,
                yRange.flat,
            )
    }

    private fun joystickRange(
        device: InputDevice,
        axis: Int,
        source: Int,
    ): InputDevice.MotionRange? =
        device.getMotionRange(axis, source)
            ?: device.motionRanges.firstOrNull { range ->
                range.axis == axis &&
                    range.source and InputDevice.SOURCE_CLASS_JOYSTICK != 0
            }

    private fun triggerAxis(
        event: MotionEvent,
        device: InputDevice,
        axis: Int,
        fallbackAxis: Int,
    ): Float {
        val range =
            joystickRange(device, axis, event.source)
                ?: joystickRange(device, fallbackAxis, event.source)
                ?: return 0f
        val value = event.getAxisValue(range.axis)
        return normalizeTriggerAxis(value, range.min, range.max)
    }

    private fun updateTrigger(
        action: ControllerAction,
        value: Float,
        left: Boolean,
    ) {
        val old = if (left) leftTriggerPressed else rightTriggerPressed
        val pressed = if (old) value >= 0.37f else value >= 0.5f
        if (old == pressed) return
        if (left) leftTriggerPressed = pressed else rightTriggerPressed = pressed
        updateControllerAction(action)
    }

    private fun updateDpad(
        action: ControllerAction,
        pressed: Boolean,
        direction: Int,
    ) {
        val old = hatPressed[direction]
        if (old == pressed) return
        hatPressed[direction] = pressed
        updateControllerAction(action)
    }

    private fun updateControllerAction(action: ControllerAction) {
        val extraPressed =
            when (action) {
                ControllerAction.AIM -> leftTriggerPressed
                ControllerAction.FIRE -> rightTriggerPressed
                ControllerAction.DPAD_UP -> hatPressed[0]
                ControllerAction.DPAD_DOWN -> hatPressed[1]
                ControllerAction.DPAD_LEFT -> hatPressed[2]
                ControllerAction.DPAD_RIGHT -> hatPressed[3]
                else -> false
            }
        val pressed = digitalControllerState[action.ordinal] || extraPressed
        if (dispatchedControllerState[action.ordinal] == pressed) return
        dispatchedControllerState[action.ordinal] = pressed
        NativeRuntime.nativeControllerButton(action.nativeValue, pressed)
    }

    private fun KeyEvent.isControllerEvent(): Boolean = device?.isPhysicalExternalController() == true

    private fun InputDevice.isPhysicalExternalController(): Boolean =
        isPhysicalExternalController(
            source = sources,
            isVirtual = isVirtual,
        )

    private fun refreshControllerDevices(): List<InputDevice> {
        val wasConnected = controllerDeviceIds.isNotEmpty()
        val candidates =
            inputManager.inputDeviceIds
                .asSequence()
                .mapNotNull { deviceId ->
                    inputManager.getInputDevice(deviceId)?.takeIf { isGameControllerSource(it.sources) }
                }.toList()
        val controllers = candidates.filter { it.isPhysicalExternalController() }
        controllerDeviceIds.clear()
        controllerDeviceIds += controllers.map { it.id }
        if (bootRequested && wasConnected != controllerDeviceIds.isNotEmpty()) {
            NativeRuntime.nativeSetControllerConnected(controllerDeviceIds.isNotEmpty())
            if (controllerDeviceIds.isEmpty()) releaseController()
        }
        val inventory =
            candidates.joinToString(separator = ";") { device ->
                val external =
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) device.isExternal.toString() else "unknown"
                "id=${device.id},source=0x${device.sources.toString(16)},virtual=${device.isVirtual}," +
                    "external=$external,number=${device.controllerNumber},accepted=${device.id in controllerDeviceIds}"
            }
        CrashDiagnostics.recordEvent(
            this,
            "controller-scan count=${controllers.size} candidates=${inventory.ifEmpty { "none" }}",
        )
        return controllers
    }

    private fun recordInputEvent(event: String) {
        if (inputDiagnosticEvents >= MAX_INPUT_DIAGNOSTIC_EVENTS) return
        inputDiagnosticEvents += 1
        CrashDiagnostics.recordEvent(this, event)
    }

    private fun updateControllerDevice(deviceId: Int) {
        val wasConnected = controllerDeviceIds.isNotEmpty()
        val isController =
            inputManager.getInputDevice(deviceId)?.isPhysicalExternalController() == true
        if (isController) {
            controllerDeviceIds += deviceId
        } else {
            controllerDeviceIds -= deviceId
        }
        val connected = controllerDeviceIds.isNotEmpty()
        if (bootRequested && wasConnected != connected) {
            NativeRuntime.nativeSetControllerConnected(connected)
        }
        if (wasConnected && !connected) {
            releaseController()
        }
    }

    private fun releaseController() {
        leftTriggerPressed = false
        rightTriggerPressed = false
        digitalControllerState.fill(false)
        dispatchedControllerState.fill(false)
        hatPressed.fill(false)
        controllerMotionRecorded = false
        NativeRuntime.nativeControllerDisconnected()
    }

    override fun onInputDeviceAdded(deviceId: Int) {
        updateControllerDevice(deviceId)
    }

    override fun onInputDeviceChanged(deviceId: Int) {
        updateControllerDevice(deviceId)
    }

    override fun onInputDeviceRemoved(deviceId: Int) {
        if (!controllerDeviceIds.remove(deviceId)) return
        val connected = controllerDeviceIds.isNotEmpty()
        if (bootRequested) NativeRuntime.nativeSetControllerConnected(connected)
        if (!connected) releaseController()
    }
}
