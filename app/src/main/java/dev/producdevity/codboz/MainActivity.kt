package dev.producdevity.codboz

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : Activity() {
    private val io = Executors.newSingleThreadExecutor()
    private lateinit var config: ConfigStore

    private val gameDir get() = File(filesDir, "game")
    private val manifestFile get() = File(gameDir, "import-manifest.json")

    private lateinit var statusTitle: TextView
    private lateinit var statusText: TextView
    private lateinit var statusMeta: TextView
    private lateinit var statusIndicator: View
    private lateinit var crashDiagnosticsPanel: View
    private lateinit var crashDiagnosticsReason: TextView
    private lateinit var diagnosticsText: TextView
    private lateinit var copyDiagnosticsButton: Button
    private lateinit var exportDiagnosticsButton: Button
    private lateinit var settingsSummary: TextView
    private lateinit var launchButton: Button
    private lateinit var importButton: Button
    private lateinit var repairButton: Button
    private var receiverRegistered = false
    private var pendingResourceAction: ResourceAction? = null
    private var gameState: GameState? = null

    @Volatile private var crashSnapshot: CrashDiagnostics.Snapshot? = null
    private val activityDestroyed = AtomicBoolean()
    private val stateRefreshPending = AtomicBoolean()
    private val stateRefreshRunning = AtomicBoolean()
    private val resourceStateReceiver =
        object : BroadcastReceiver() {
            override fun onReceive(
                context: Context?,
                intent: Intent?,
            ) {
                val download = ResourceDownloadService.state(this@MainActivity)
                val current = gameState
                if (download.status == ResourceDownloadService.STATUS_DOWNLOADING && current != null) {
                    render(current, download)
                } else if (!stateRefreshRunning.get()) {
                    requestGameStateRefresh()
                }
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.windowInsetsController?.show(WindowInsets.Type.systemBars())
        }

        config = ConfigStore(File(filesDir, "config"))
        config.ensureDefault()

        statusTitle = findViewById(R.id.status_title)
        statusText = findViewById(R.id.status_text)
        statusMeta = findViewById(R.id.status_meta)
        statusIndicator = findViewById(R.id.status_indicator)
        crashDiagnosticsPanel = findViewById(R.id.crash_diagnostics_panel)
        crashDiagnosticsReason = findViewById(R.id.crash_diagnostics_reason)
        diagnosticsText = findViewById(R.id.diagnostics_text)
        copyDiagnosticsButton = findViewById(R.id.copy_diagnostics_button)
        exportDiagnosticsButton = findViewById(R.id.export_diagnostics_button)
        settingsSummary = findViewById(R.id.settings_summary)
        launchButton = findViewById(R.id.launch_button)
        importButton = findViewById(R.id.import_button)
        repairButton = findViewById(R.id.repair_button)

        importButton.setOnClickListener {
            @Suppress("DEPRECATION")
            startActivityForResult(
                Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "application/vnd.android.package-archive"
                },
                REQUEST_PICK_APK,
            )
        }
        findViewById<Button>(R.id.settings_button).setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
        findViewById<Button>(R.id.runtime_details_button).setOnClickListener {
            val visibility = if (diagnosticsText.visibility == View.VISIBLE) View.GONE else View.VISIBLE
            diagnosticsText.visibility = visibility
            copyDiagnosticsButton.visibility = visibility
            exportDiagnosticsButton.visibility = visibility
            if (visibility == View.VISIBLE) {
                diagnosticsText.setText(R.string.status_checking_detail)
                loadDiagnostics()
            }
        }
        copyDiagnosticsButton.setOnClickListener { copyDiagnostics() }
        exportDiagnosticsButton.setOnClickListener { createDiagnosticsReport() }
        findViewById<Button>(R.id.crash_copy_diagnostics_button).setOnClickListener { copyDiagnostics() }
        findViewById<Button>(R.id.crash_save_diagnostics_button).setOnClickListener {
            createDiagnosticsReport()
        }
        launchButton.setOnClickListener { handlePrimaryAction() }
        repairButton.setOnClickListener { confirmResourceRepair() }
        findViewById<Button>(R.id.third_party_notices_button).setOnClickListener {
            AlertDialog
                .Builder(this)
                .setTitle(R.string.third_party_notices)
                .setMessage(
                    listOf(
                        "project_notice.txt",
                        "project_license.txt",
                        "third_party_notices.txt",
                        "ndk_llvm_notice.txt",
                    ).joinToString("\n\n") { asset ->
                        assets.open(asset).bufferedReader().use { it.readText() }
                    },
                ).setPositiveButton(android.R.string.ok, null)
                .show()
        }

        refreshSettingsSummary()
        CrashDiagnostics.setProcessState(this, "launcher")
        loadCrashSnapshot()
        animateEntrance()
    }

    override fun onResume() {
        super.onResume()
        if (::config.isInitialized) {
            CrashDiagnostics.setProcessState(this, "launcher")
            refreshSettingsSummary()
            requestGameStateRefresh()
        }
    }

    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    override fun onStart() {
        super.onStart()
        val filter = IntentFilter(ResourceDownloadService.ACTION_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(resourceStateReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(resourceStateReceiver, filter)
        }
        receiverRegistered = true
    }

    override fun onStop() {
        if (receiverRegistered) {
            unregisterReceiver(resourceStateReceiver)
            receiverRegistered = false
        }
        super.onStop()
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?,
    ) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == REQUEST_EXPORT_DIAGNOSTICS) {
            if (resultCode == RESULT_OK && uri != null) exportDiagnostics(uri)
            return
        }
        if (requestCode != REQUEST_PICK_APK) return
        if (resultCode != RESULT_OK || uri == null) return
        setImporting(true)
        io.execute { runImport(uri) }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_NOTIFICATIONS) {
            pendingResourceAction?.let(::performResourceAction)
            pendingResourceAction = null
        }
    }

    override fun onDestroy() {
        activityDestroyed.set(true)
        io.shutdownNow()
        super.onDestroy()
    }

    private fun runImport(uri: android.net.Uri) {
        CrashDiagnostics.setProcessState(this, "apk-import")
        val temp = File(cacheDir, "import.apk")
        val outcome =
            runCatching {
                contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Cannot open the selected APK" }
                    temp.outputStream().use { output ->
                        copyWithLimit(input, output, MAX_SOURCE_APK_BYTES)
                    }
                }
                JSONObject(NativeRuntime.nativeImportApk(temp.absolutePath, gameDir.absolutePath))
            }
        temp.delete()
        runOnUiThread {
            if (activityDestroyed.get()) return@runOnUiThread
            CrashDiagnostics.setProcessState(this, "launcher")
            setImporting(false)
            outcome.fold(
                onSuccess = { result ->
                    if (result.optBoolean("ok")) {
                        ResourceDownloadService.resetState(this)
                        gameState = null
                        requestGameStateRefresh()
                        toast("Game data imported")
                    } else {
                        requestGameStateRefresh()
                        showImportFailure(result.optString("error").ifBlank { "Unknown error" })
                    }
                },
                onFailure = { error ->
                    requestGameStateRefresh()
                    showImportFailure(error.message ?: "Unknown error")
                },
            )
        }
    }

    private fun setImporting(importing: Boolean) {
        importButton.isEnabled = !importing
        launchButton.isEnabled = !importing && gameState?.imported == true
        repairButton.isEnabled = !importing && gameState?.imported == true
        if (importing) {
            statusTitle.setText(R.string.importing)
            statusText.setText(R.string.status_checking_detail)
            statusMeta.visibility = View.GONE
            statusIndicator.setBackgroundResource(R.drawable.status_indicator_off)
        }
    }

    private fun requestGameStateRefresh() {
        if (activityDestroyed.get()) return
        stateRefreshPending.set(true)
        if (!stateRefreshRunning.compareAndSet(false, true)) return
        try {
            io.execute {
                try {
                    while (stateRefreshPending.getAndSet(false)) {
                        val state = loadGameState()
                        runOnUiThread {
                            if (activityDestroyed.get()) return@runOnUiThread
                            gameState = state
                            render(state, ResourceDownloadService.state(this))
                        }
                    }
                } finally {
                    stateRefreshRunning.set(false)
                    if (stateRefreshPending.get() && !activityDestroyed.get()) requestGameStateRefresh()
                }
            }
        } catch (_: RejectedExecutionException) {
            stateRefreshRunning.set(false)
        }
    }

    private fun loadGameState(): GameState {
        val prepared = gameDir.exists() && NativeRuntime.nativeIsPrepared(gameDir.absolutePath)
        val imported = prepared || (gameDir.exists() && NativeRuntime.nativeIsImported(gameDir.absolutePath))
        return GameState(imported, prepared, readManifest())
    }

    private fun render(
        state: GameState,
        download: ResourceDownloadService.State,
    ) {
        val manifest = state.manifest
        if (state.imported && manifest != null &&
            download.status == ResourceDownloadService.STATUS_DOWNLOADING
        ) {
            val downloadedMiB = download.downloaded / (1024 * 1024)
            val totalMiB = download.total / (1024 * 1024)
            val percent =
                if (download.total == 0L) 0 else (download.downloaded * 100 / download.total).toInt()
            statusTitle.setText(R.string.status_downloading)
            statusText.setText(R.string.preparing_resources)
            statusMeta.text =
                getString(
                    R.string.download_meta,
                    download.archive ?: getString(R.string.game_data_download),
                    percent,
                    downloadedMiB,
                    totalMiB,
                )
            statusMeta.visibility = View.VISIBLE
            statusIndicator.setBackgroundResource(R.drawable.status_indicator_off)
        } else if (state.prepared && manifest != null) {
            statusTitle.setText(R.string.status_ready)
            statusText.setText(R.string.status_ready_detail)
            statusMeta.text =
                getString(
                    R.string.import_meta,
                    manifest.getString("source_version"),
                    manifest.optString("sha256").take(12).uppercase(),
                    manifest.optInt("s3e_symbols"),
                )
            statusMeta.visibility = View.VISIBLE
            statusIndicator.setBackgroundResource(R.drawable.status_indicator_on)
        } else if (state.imported && manifest != null) {
            when (download.status) {
                ResourceDownloadService.STATUS_ERROR -> {
                    statusTitle.setText(R.string.status_download_error)
                    statusText.text = download.error ?: getString(R.string.download_failed)
                    statusMeta.visibility = View.GONE
                }

                else -> {
                    statusTitle.setText(R.string.status_apk_ready)
                    statusText.setText(R.string.status_apk_ready_detail)
                    statusMeta.text =
                        getString(
                            R.string.import_meta,
                            manifest.getString("source_version"),
                            manifest.optString("sha256").take(12).uppercase(),
                            manifest.optInt("s3e_symbols"),
                        )
                    statusMeta.visibility = View.VISIBLE
                }
            }
            statusIndicator.setBackgroundResource(R.drawable.status_indicator_off)
        } else {
            statusTitle.setText(R.string.status_missing)
            statusText.setText(R.string.status_missing_detail)
            statusMeta.visibility = View.GONE
            statusIndicator.setBackgroundResource(R.drawable.status_indicator_off)
        }
        launchButton.isEnabled = state.imported
        launchButton.setText(
            when {
                download.status == ResourceDownloadService.STATUS_DOWNLOADING -> R.string.cancel_download
                state.prepared -> R.string.launch_game
                download.status == ResourceDownloadService.STATUS_ERROR -> R.string.retry_download
                else -> R.string.download_game_data
            },
        )
        importButton.isEnabled = download.status != ResourceDownloadService.STATUS_DOWNLOADING
        importButton.setText(if (state.imported) R.string.import_apk else R.string.import_apk_short)
        repairButton.visibility = if (state.imported) View.VISIBLE else View.GONE
        repairButton.isEnabled = download.status != ResourceDownloadService.STATUS_DOWNLOADING
    }

    private fun handlePrimaryAction() {
        val current = gameState ?: return
        if (!current.imported) return
        if (ResourceDownloadService.state(this).status == ResourceDownloadService.STATUS_DOWNLOADING) {
            ResourceDownloadService.cancel(this)
            return
        }
        if (current.prepared) {
            startActivity(
                Intent(this, GameActivity::class.java).apply {
                    addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    putExtra(GameActivity.EXTRA_GAME_DIR, gameDir.absolutePath)
                },
            )
            return
        }
        startResourceAction(ResourceAction.DOWNLOAD)
    }

    private fun confirmResourceRepair() {
        if (gameState?.imported != true) return
        AlertDialog
            .Builder(this)
            .setTitle(R.string.repair_game_data)
            .setMessage(R.string.repair_game_data_detail)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.redownload) { _, _ ->
                startResourceAction(ResourceAction.REPAIR)
            }.show()
    }

    private fun startResourceAction(action: ResourceAction) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            pendingResourceAction = action
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
            return
        }
        performResourceAction(action)
    }

    private fun performResourceAction(action: ResourceAction) {
        when (action) {
            ResourceAction.DOWNLOAD -> ResourceDownloadService.start(this)
            ResourceAction.REPAIR -> ResourceDownloadService.repair(this)
        }
    }

    private fun readManifest(): JSONObject? =
        if (!manifestFile.exists()) null else runCatching { JSONObject(manifestFile.readText()) }.getOrNull()

    private fun buildDiagnostics(manifest: JSONObject?): String {
        val cpu = runCatching { NativeRuntime.nativeCpuSelfTest() }.getOrElse { it.toString() }
        val displayMetrics = resources.displayMetrics
        val guestDisplay =
            DisplayGeometry.guestSurface(displayMetrics.widthPixels, displayMetrics.heightPixels)
        return buildString {
            appendLine(NativeRuntime.nativeVersion())
            appendLine("host abi    ${Build.SUPPORTED_ABIS.joinToString()}")
            appendLine("page size   ${NativeRuntime.nativePageSize()}")
            appendLine("guest cpu   $cpu")
            appendLine(
                "display     ${guestDisplay.width} x ${guestDisplay.height}",
            )
            appendLine("graphics    EGL / OpenGL ES 2")
            appendLine("imports     ${manifest?.optInt("s3e_symbols") ?: "not loaded"}")
            append("source      ${manifest?.optString("sha256")?.take(16) ?: "not loaded"}")
        }
    }

    private fun loadDiagnostics() {
        val manifest = gameState?.manifest
        io.execute {
            val snapshot = crashSnapshot ?: CrashDiagnostics.collect(this).also { crashSnapshot = it }
            val text =
                buildString {
                    append(buildDiagnostics(manifest))
                    snapshot.latestFailureLabel?.let {
                        appendLine()
                        appendLine()
                        append("previous exit $it")
                    }
                }
            runOnUiThread {
                if (!activityDestroyed.get() && diagnosticsText.visibility == View.VISIBLE) {
                    diagnosticsText.text = text
                }
            }
        }
    }

    private fun loadCrashSnapshot() {
        io.execute {
            val snapshot = CrashDiagnostics.collect(this)
            crashSnapshot = snapshot
            runOnUiThread {
                if (activityDestroyed.get()) return@runOnUiThread
                crashDiagnosticsPanel.visibility =
                    if (snapshot.hasUnacknowledgedFailure) View.VISIBLE else View.GONE
                crashDiagnosticsReason.text =
                    getString(
                        R.string.previous_failure_detail,
                        failureDescription(snapshot.latestFailureLabel),
                    )
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun createDiagnosticsReport() {
        startActivityForResult(
            Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "text/plain"
                putExtra(Intent.EXTRA_TITLE, "codboz-diagnostics.txt")
            },
            REQUEST_EXPORT_DIAGNOSTICS,
        )
    }

    private fun copyDiagnostics() {
        io.execute {
            val outcome = runCatching { buildDiagnosticsReport() }
            runOnUiThread {
                if (activityDestroyed.get()) return@runOnUiThread
                outcome.fold(
                    onSuccess = { report ->
                        getSystemService(ClipboardManager::class.java).setPrimaryClip(
                            ClipData.newPlainText(getString(R.string.diagnostics_clip_label), report),
                        )
                        acknowledgeDiagnostics()
                        toast(getString(R.string.diagnostics_copied))
                    },
                    onFailure = { toast(getString(R.string.diagnostics_build_failed)) },
                )
            }
        }
    }

    private fun exportDiagnostics(uri: android.net.Uri) {
        io.execute {
            val outcome =
                runCatching {
                    val report = buildDiagnosticsReport()
                    contentResolver.openOutputStream(uri, "wt").use { output ->
                        requireNotNull(output) { getString(R.string.diagnostics_write_failed) }
                        output.write(report.toByteArray(Charsets.UTF_8))
                    }
                }
            runOnUiThread {
                if (activityDestroyed.get()) return@runOnUiThread
                toast(
                    outcome.fold(
                        onSuccess = {
                            acknowledgeDiagnostics()
                            getString(R.string.diagnostics_exported)
                        },
                        onFailure = { getString(R.string.diagnostics_write_failed) },
                    ),
                )
            }
        }
    }

    private fun buildDiagnosticsReport(): String {
        val state = loadGameState()
        val snapshot = CrashDiagnostics.collect(this).also { crashSnapshot = it }
        val displayMetrics = resources.displayMetrics
        val guestDisplay =
            DisplayGeometry.guestSurface(displayMetrics.widthPixels, displayMetrics.heightPixels)
        val refreshRate =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                display?.refreshRate ?: 0f
            } else {
                @Suppress("DEPRECATION")
                windowManager.defaultDisplay.refreshRate
            }
        val configValues = config.read()
        val sensitiveValues =
            listOfNotNull(
                configValues["player_name"],
                configValues["multiplayer_server"],
            )
        return DiagnosticsReport.create(
            appVersion = packageVersion(),
            hostApkHash = sha256(File(applicationInfo.sourceDir)),
            runtimeVersion = NativeRuntime.nativeVersion(),
            sdk = Build.VERSION.SDK_INT,
            androidRelease = Build.VERSION.RELEASE,
            deviceManufacturer = Build.MANUFACTURER,
            deviceModel = Build.MODEL,
            hostAbis = Build.SUPPORTED_ABIS.toList(),
            pageSize = NativeRuntime.nativePageSize(),
            displayWidthPx = displayMetrics.widthPixels,
            displayHeightPx = displayMetrics.heightPixels,
            densityDpi = displayMetrics.densityDpi,
            refreshRateHz = refreshRate,
            memoryClassMb = getSystemService(ActivityManager::class.java).memoryClass,
            guestWidth = guestDisplay.width,
            guestHeight = guestDisplay.height,
            cpuSelfTest = NativeRuntime.nativeCpuSelfTest(),
            imported = state.imported,
            prepared = state.prepared,
            sourceHash = state.manifest?.optString("sha256")?.takeIf { it.isNotBlank() },
            importCount = state.manifest?.optInt("s3e_symbols")?.takeIf { it > 0 },
            voiceChatEnabled = configValues["voice_chat_enabled"] == "1",
            microphonePermissionGranted =
                checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED,
            runtimeReport = File(filesDir, "runtime-report.txt"),
            exits = snapshot.exits,
            javaCrash = snapshot.javaCrash,
            recentEvents = snapshot.recentEvents,
            sensitiveValues = sensitiveValues,
        )
    }

    private fun acknowledgeDiagnostics() {
        val snapshot = crashSnapshot ?: return
        CrashDiagnostics.acknowledge(this, snapshot.attentionTimestampMillis)
    }

    private fun failureDescription(reason: String?): String =
        when (reason) {
            "anr" -> getString(R.string.failure_anr)
            "java-crash" -> getString(R.string.failure_java_crash)
            "native-crash" -> getString(R.string.failure_native_crash)
            "low-memory" -> getString(R.string.failure_low_memory)
            "signal" -> getString(R.string.failure_signal)
            "initialization-failure" -> getString(R.string.failure_initialization)
            "excessive-resource-use" -> getString(R.string.failure_resources)
            else -> getString(R.string.unknown_failure)
        }

    private fun sha256(file: File): String =
        runCatching {
            val digest = MessageDigest.getInstance("SHA-256")
            file.inputStream().buffered().use { input ->
                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    digest.update(buffer, 0, count)
                }
            }
            digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
        }.getOrDefault("unavailable")

    @Suppress("DEPRECATION")
    private fun packageVersion(): String = packageManager.getPackageInfo(packageName, 0).versionName ?: "unknown"

    private fun refreshSettingsSummary() {
        val values = config.read()
        val player = values["player_name"] ?: "Player"
        val online = !values["multiplayer_server"].isNullOrBlank()
        val voice = values["voice_chat_enabled"] == "1"
        settingsSummary.text =
            getString(
                R.string.settings_summary,
                player,
                buildString {
                    append(getString(if (online) R.string.online else R.string.offline))
                    if (voice) append("  •  ").append(getString(R.string.voice_chat))
                },
            )
    }

    private fun showImportFailure(message: String) {
        AlertDialog
            .Builder(this)
            .setTitle(R.string.import_failed_title)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun animateEntrance() {
        val content = findViewById<View>(R.id.launcher_content)
        content.alpha = 0f
        content.translationY = 20f * resources.displayMetrics.density
        content
            .animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(360)
            .start()
    }

    private fun toast(message: String) = Toast.makeText(this, message, Toast.LENGTH_SHORT).show()

    private data class GameState(
        val imported: Boolean,
        val prepared: Boolean,
        val manifest: JSONObject?,
    )

    private enum class ResourceAction {
        DOWNLOAD,
        REPAIR,
    }

    companion object {
        private const val REQUEST_PICK_APK = 41
        private const val REQUEST_NOTIFICATIONS = 42
        private const val REQUEST_EXPORT_DIAGNOSTICS = 43
        private const val MAX_SOURCE_APK_BYTES = 256L * 1024L * 1024L
    }
}
