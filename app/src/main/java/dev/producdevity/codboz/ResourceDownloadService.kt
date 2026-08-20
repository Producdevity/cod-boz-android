package dev.producdevity.codboz

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.IBinder
import android.os.StatFs
import android.system.Os
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

class ResourceDownloadService : Service() {
    private val worker = Executors.newSingleThreadExecutor()
    private val cancelled = AtomicBoolean()
    private val running = AtomicBoolean()
    private val activeConnection = AtomicReference<HttpURLConnection?>()
    private val sourceProfile by lazy { SourceProfile.load(applicationContext) }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(
        intent: Intent?,
        flags: Int,
        startId: Int,
    ): Int {
        if (intent?.action == ACTION_CANCEL) {
            cancelled.set(true)
            activeConnection.getAndSet(null)?.disconnect()
            if (!running.get()) {
                updateState(State(STATUS_IDLE, null, 0, sourceProfile.totalResourceSize, null))
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf(startId)
            }
            return START_NOT_STICKY
        }
        val repair = intent?.action == ACTION_REPAIR
        if (!running.compareAndSet(false, true)) return START_NOT_STICKY
        CrashDiagnostics.setProcessState(this, if (repair) "resource-repair" else "resource-download")
        cancelled.set(false)
        updateState(State(STATUS_DOWNLOADING, null, 0, sourceProfile.totalResourceSize, null))
        startForeground(NOTIFICATION_ID, notification(getString(R.string.preparing_resources), 0))
        worker.execute { prepareResources(repair) }
        return START_REDELIVER_INTENT
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        cancelled.set(true)
        activeConnection.getAndSet(null)?.disconnect()
        worker.shutdownNow()
        super.onDestroy()
    }

    private fun prepareResources(repair: Boolean) {
        var processState = "resource-download-failed"
        try {
            val gameDir = File(filesDir, "game")
            require(NativeRuntime.nativeIsImported(gameDir.absolutePath)) { "Import the supported APK first" }
            val manifest = JSONObject(File(gameDir, "import-manifest.json").readText())
            val baseUrl = manifest.getString("resource_base_url")
            require(baseUrl == sourceProfile.resourceBaseUrl) {
                "The imported APK has an unsupported resource source"
            }
            if (repair) ResourceFiles.clearDownloads(gameDir, sourceProfile.archives)

            val assets = File(gameDir, "assets")
            assets.mkdirs()
            var completed = 0L
            for (archive in sourceProfile.archives) {
                check(!cancelled.get()) { "Download cancelled" }
                prepareArchive(baseUrl, assets, archive, completed)
                completed += archive.size
            }
            writeResourceManifest(gameDir)
            updateState(
                State(
                    STATUS_READY,
                    null,
                    sourceProfile.totalResourceSize,
                    sourceProfile.totalResourceSize,
                    null,
                ),
            )
            notify(notification(getString(R.string.resources_ready), 100))
            processState = "resource-download-finished"
        } catch (error: Exception) {
            val message = error.message?.takeIf { it.isNotBlank() } ?: getString(R.string.download_failed)
            val status = if (cancelled.get()) STATUS_IDLE else STATUS_ERROR
            updateState(
                State(
                    status,
                    null,
                    0,
                    sourceProfile.totalResourceSize,
                    message.takeIf { status == STATUS_ERROR },
                ),
            )
            if (status == STATUS_ERROR) {
                notify(notification(message, null))
            }
            if (cancelled.get()) processState = "resource-download-cancelled"
        } finally {
            CrashDiagnostics.setProcessState(this, processState)
            running.set(false)
            if (cancelled.get()) {
                stopForeground(STOP_FOREGROUND_REMOVE)
            } else {
                stopForeground(STOP_FOREGROUND_DETACH)
            }
            stopSelf()
        }
    }

    private fun prepareArchive(
        baseUrl: String,
        assets: File,
        archive: ResourceArchive,
        completed: Long,
    ) {
        val target = File(assets, archive.name)
        val partial = File(assets, "${archive.name}.part")
        if (target.length() == archive.size && sha256(target) == archive.sha256) {
            Os.chmod(target.absolutePath, READ_ONLY_MODE)
            partial.delete()
            return
        }
        if (target.exists() && !target.delete()) error("Cannot replace ${archive.name}")

        if (partial.length() > archive.size && !partial.delete()) error("Cannot reset ${archive.name}")
        if (partial.length() < archive.size) {
            requireAvailableSpace(assets, archive.name, archive.size - partial.length())
            download(baseUrl + archive.name, partial, archive, completed)
        }
        check(partial.length() == archive.size) { "${archive.name} has an unexpected size" }
        if (sha256(partial) != archive.sha256) {
            partial.delete()
            error("${archive.name} failed checksum verification")
        }
        Os.rename(partial.absolutePath, target.absolutePath)
        Os.chmod(target.absolutePath, READ_ONLY_MODE)
    }

    private fun download(
        url: String,
        partial: File,
        archive: ResourceArchive,
        completed: Long,
    ) {
        var offset = partial.length()
        var connection = openTrackedConnection(url, offset)
        if (offset > 0) {
            val responseCode =
                try {
                    connection.responseCode
                } catch (error: Exception) {
                    closeConnection(connection)
                    throw error
                }
            if (responseCode != HttpURLConnection.HTTP_PARTIAL) {
                closeConnection(connection)
                FileOutputStream(partial, false).use { }
                offset = 0
                connection = openTrackedConnection(url, 0)
            }
        }
        try {
            val expectedCode = if (offset == 0L) HttpURLConnection.HTTP_OK else HttpURLConnection.HTTP_PARTIAL
            check(connection.responseCode == expectedCode) {
                "Resource server returned HTTP ${connection.responseCode}"
            }
            val expectedLength = archive.size - offset
            check(connection.contentLengthLong == expectedLength) {
                "${archive.name} has an unexpected server size"
            }
            connection.inputStream.use { input ->
                FileOutputStream(partial, offset > 0).use { output ->
                    val buffer = ByteArray(1024 * 1024)
                    var downloaded = offset
                    var lastPublished = offset
                    publishProgress(archive.name, completed + downloaded)
                    while (true) {
                        check(!cancelled.get()) { "Download cancelled" }
                        val count = input.read(buffer)
                        if (count < 0) break
                        check(downloaded + count <= archive.size) {
                            "${archive.name} exceeded its expected size"
                        }
                        output.write(buffer, 0, count)
                        downloaded += count
                        if (downloaded - lastPublished >= buffer.size) {
                            publishProgress(archive.name, completed + downloaded)
                            lastPublished = downloaded
                        }
                    }
                    output.fd.sync()
                }
            }
        } finally {
            closeConnection(connection)
        }
    }

    private fun openTrackedConnection(
        url: String,
        offset: Long,
    ): HttpURLConnection = openConnection(url, offset).also(activeConnection::set)

    private fun closeConnection(connection: HttpURLConnection) {
        activeConnection.compareAndSet(connection, null)
        connection.disconnect()
    }

    private fun requireAvailableSpace(
        directory: File,
        archive: String,
        required: Long,
    ) {
        val available = StatFs(directory.absolutePath).availableBytes
        val minimum = required + STORAGE_RESERVE
        check(available >= minimum) {
            val requiredMiB = (minimum + MIB - 1) / MIB
            "$archive needs $requiredMiB MiB of free storage"
        }
    }

    private fun openConnection(
        url: String,
        offset: Long,
    ): HttpURLConnection =
        (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 20_000
            readTimeout = 30_000
            instanceFollowRedirects = false
            setRequestProperty("Accept-Encoding", "identity")
            if (offset > 0) setRequestProperty("Range", "bytes=$offset-")
        }

    private fun publishProgress(
        archive: String,
        downloaded: Long,
    ) {
        val state = State(STATUS_DOWNLOADING, archive, downloaded, sourceProfile.totalResourceSize, null)
        updateState(state)
        val percent =
            ((downloaded * 100) / sourceProfile.totalResourceSize).toInt().coerceIn(0, 100)
        notify(notification(archive, percent))
    }

    private fun writeResourceManifest(gameDir: File) {
        val json = NativeRuntime.nativeResourceManifest()
        val file = AtomicFile(File(gameDir, "resource-manifest.json"))
        val output = file.startWrite()
        try {
            output.write(json.toByteArray())
            file.finishWrite(output)
        } catch (error: Exception) {
            file.failWrite(output)
            throw error
        }
        Os.chmod(file.baseFile.absolutePath, READ_ONLY_MODE)
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered().use { input ->
            val buffer = ByteArray(1024 * 1024)
            while (true) {
                check(!cancelled.get()) { "Download cancelled" }
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }

    private fun updateState(state: State) {
        getSharedPreferences(PREFERENCES, MODE_PRIVATE)
            .edit()
            .putString(KEY_STATUS, state.status)
            .putString(KEY_ARCHIVE, state.archive)
            .putLong(KEY_DOWNLOADED, state.downloaded)
            .putLong(KEY_TOTAL, state.total)
            .putString(KEY_ERROR, state.error)
            .apply()
        sendBroadcast(Intent(ACTION_STATE_CHANGED).setPackage(packageName))
    }

    private fun notification(
        message: String,
        progress: Int?,
    ): Notification {
        val contentIntent =
            PendingIntent.getActivity(
                this,
                0,
                Intent(this, MainActivity::class.java),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
        val cancelIntent =
            PendingIntent.getService(
                this,
                1,
                Intent(this, ResourceDownloadService::class.java).setAction(ACTION_CANCEL),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
        val builder =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                Notification.Builder(this, CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION")
                Notification.Builder(this)
            }
        return builder
            .setSmallIcon(R.drawable.ic_launcher_monochrome)
            .setContentTitle(getString(R.string.game_data_download))
            .setContentText(message)
            .setContentIntent(contentIntent)
            .setOnlyAlertOnce(true)
            .setOngoing(progress != null && progress < 100)
            .apply {
                if (progress != null && progress < 100) {
                    setProgress(100, progress, false)
                    @Suppress("DEPRECATION")
                    addAction(0, getString(R.string.cancel_download), cancelIntent)
                }
            }.build()
    }

    @SuppressLint("MissingPermission")
    private fun notify(notification: Notification) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED
        ) {
            notificationManager.notify(NOTIFICATION_ID, notification)
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            notificationManager.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.game_data_download),
                    NotificationManager.IMPORTANCE_LOW,
                ),
            )
        }
    }

    private val notificationManager get() = getSystemService(NotificationManager::class.java)

    data class State(
        val status: String,
        val archive: String?,
        val downloaded: Long,
        val total: Long,
        val error: String?,
    )

    companion object {
        const val ACTION_STATE_CHANGED = "dev.producdevity.codboz.RESOURCE_STATE_CHANGED"
        private const val ACTION_START = "dev.producdevity.codboz.START_RESOURCE_DOWNLOAD"
        private const val ACTION_CANCEL = "dev.producdevity.codboz.CANCEL_RESOURCE_DOWNLOAD"
        private const val ACTION_REPAIR = "dev.producdevity.codboz.REPAIR_RESOURCE_DOWNLOAD"
        private const val PREFERENCES = "resource_download"
        private const val KEY_STATUS = "status"
        private const val KEY_ARCHIVE = "archive"
        private const val KEY_DOWNLOADED = "downloaded"
        private const val KEY_TOTAL = "total"
        private const val KEY_ERROR = "error"
        const val STATUS_IDLE = "idle"
        const val STATUS_DOWNLOADING = "downloading"
        const val STATUS_READY = "ready"
        const val STATUS_ERROR = "error"
        private const val CHANNEL_ID = "game-data"
        private const val NOTIFICATION_ID = 1138
        private const val MIB = 1024L * 1024L
        private const val READ_ONLY_MODE = 0x100
        private const val STORAGE_RESERVE = 32L * MIB

        fun start(context: Context) {
            val intent = Intent(context, ResourceDownloadService::class.java).setAction(ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun cancel(context: Context) {
            context.startService(Intent(context, ResourceDownloadService::class.java).setAction(ACTION_CANCEL))
        }

        fun repair(context: Context) {
            val intent = Intent(context, ResourceDownloadService::class.java).setAction(ACTION_REPAIR)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun state(context: Context): State {
            val preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            val storedTotal = preferences.getLong(KEY_TOTAL, 0)
            val totalSize = if (storedTotal > 0) storedTotal else SourceProfile.load(context).totalResourceSize
            return State(
                preferences.getString(KEY_STATUS, STATUS_IDLE) ?: STATUS_IDLE,
                preferences.getString(KEY_ARCHIVE, null),
                preferences.getLong(KEY_DOWNLOADED, 0),
                preferences.getLong(KEY_TOTAL, totalSize),
                preferences.getString(KEY_ERROR, null),
            )
        }

        fun resetState(context: Context) {
            context
                .getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
                .edit()
                .clear()
                .apply()
            context.sendBroadcast(Intent(ACTION_STATE_CHANGED).setPackage(context.packageName))
        }
    }
}
