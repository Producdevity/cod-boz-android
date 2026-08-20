package dev.producdevity.codboz

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.widget.Button
import android.widget.EditText
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.SecureRandom
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class SettingsActivity : Activity() {
    private val io = Executors.newSingleThreadExecutor()
    private lateinit var config: ConfigStore
    private lateinit var playerName: EditText
    private lateinit var onlineSwitch: Switch
    private lateinit var voiceChatSwitch: Switch
    private lateinit var server: EditText
    private lateinit var testButton: Button
    private lateinit var status: TextView
    private val destroyed = AtomicBoolean()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.decorView.windowInsetsController?.show(WindowInsets.Type.systemBars())
        }

        config = ConfigStore(File(filesDir, "config"))
        config.ensureDefault()
        playerName = findViewById(R.id.settings_player_name)
        onlineSwitch = findViewById(R.id.online_switch)
        voiceChatSwitch = findViewById(R.id.voice_chat_switch)
        server = findViewById(R.id.multiplayer_server)
        testButton = findViewById(R.id.test_connection_button)
        status = findViewById(R.id.settings_status)

        findViewById<Button>(R.id.back_button).setOnClickListener { finish() }
        findViewById<Button>(R.id.restore_server_button).setOnClickListener {
            onlineSwitch.isChecked = true
            server.setText(ConfigDocument.DEFAULT_SERVER)
            showStatus(R.string.server_restored, false)
        }
        findViewById<Button>(R.id.import_config_button).setOnClickListener { openConfig() }
        findViewById<Button>(R.id.export_config_button).setOnClickListener { createConfig() }
        findViewById<Button>(R.id.save_settings_button).setOnClickListener { save() }
        testButton.setOnClickListener { testConnection() }
        onlineSwitch.setOnCheckedChangeListener { _, _ -> updateOnlineControls() }

        load()
        animateEntrance()
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?,
    ) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data ?: return
        if (resultCode != RESULT_OK) return
        when (requestCode) {
            REQUEST_IMPORT_CONFIG -> importConfig(uri)
            REQUEST_EXPORT_CONFIG -> exportConfig(uri)
        }
    }

    override fun onDestroy() {
        destroyed.set(true)
        io.shutdownNow()
        super.onDestroy()
    }

    private fun load() {
        val values = config.read()
        playerName.setText(values["player_name"] ?: "Player")
        val configuredServer = values["multiplayer_server"].orEmpty()
        onlineSwitch.isChecked = configuredServer.isNotEmpty()
        voiceChatSwitch.isChecked = values["voice_chat_enabled"] == "1"
        server.setText(configuredServer.ifEmpty { ConfigDocument.DEFAULT_SERVER })
        updateOnlineControls()
    }

    private fun save() {
        val name = playerName.text.toString().trim()
        val configuredServer = if (onlineSwitch.isChecked) server.text.toString().trim() else ""
        if (!ConfigDocument.validPlayerName(name)) {
            showStatus(R.string.player_name_rules, true)
            playerName.requestFocus()
            return
        }
        if (!ConfigDocument.validMultiplayerServer(configuredServer) ||
            (onlineSwitch.isChecked && configuredServer.isEmpty())
        ) {
            showStatus(R.string.server_rules, true)
            server.requestFocus()
            return
        }
        if (voiceChatSwitch.isChecked &&
            checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_RECORD_AUDIO)
            return
        }

        runCatching {
            config.update(
                mapOf(
                    "player_name" to name,
                    "multiplayer_server" to configuredServer,
                    "voice_chat_enabled" to if (voiceChatSwitch.isChecked) "1" else "0",
                ),
            )
        }.fold(
            onSuccess = {
                showStatus(R.string.settings_saved, false)
                Toast.makeText(this, R.string.settings_saved_short, Toast.LENGTH_SHORT).show()
            },
            onFailure = { showStatus(it.message ?: getString(R.string.config_write_failed), true) },
        )
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQUEST_RECORD_AUDIO) return
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
            save()
        } else {
            voiceChatSwitch.isChecked = false
            showStatus(R.string.microphone_permission_required, true)
        }
    }

    private fun updateOnlineControls() {
        val enabled = onlineSwitch.isChecked
        server.isEnabled = enabled
        testButton.isEnabled = enabled
    }

    private fun testConnection() {
        val host = server.text.toString().trim()
        if (!ConfigDocument.validMultiplayerServer(host) || host.isEmpty()) {
            showStatus(R.string.server_rules, true)
            return
        }
        testButton.isEnabled = false
        showStatus(R.string.testing_connection, false)
        io.execute {
            val result = runConnectionTest(host)
            runOnUiThread {
                if (destroyed.get()) return@runOnUiThread
                testButton.isEnabled = onlineSwitch.isChecked
                if (result.address == null) {
                    showStatus(result.error ?: getString(R.string.dns_failed), true)
                } else {
                    status.setTextColor(getColor(if (result.tcp) R.color.signal_green else R.color.muted))
                    status.text =
                        getString(
                            R.string.connection_result,
                            result.address.hostAddress,
                            getString(if (result.tcp) R.string.reachable else R.string.unreachable),
                            getString(if (result.udp) R.string.available else R.string.unavailable),
                        )
                }
            }
        }
    }

    private fun runConnectionTest(host: String): ConnectionResult =
        runCatching {
            val addresses = InetAddress.getAllByName(host).filterIsInstance<Inet4Address>()
            if (addresses.isEmpty()) return ConnectionResult(error = getString(R.string.ipv4_required))
            addresses
                .map { address ->
                    ConnectionResult(
                        address = address,
                        tcp = probeTcp(address),
                        udp = probeStun(address),
                    )
                }.maxBy { (if (it.tcp) 2 else 0) + (if (it.udp) 1 else 0) }
        }.getOrElse { ConnectionResult(error = it.message ?: getString(R.string.dns_failed)) }

    private fun probeTcp(address: Inet4Address): Boolean =
        runCatching {
            Socket().use { it.connect(InetSocketAddress(address, GAME_PORT), TEST_TIMEOUT_MS) }
        }.isSuccess

    private fun probeStun(address: Inet4Address): Boolean =
        runCatching {
            val transaction = ByteArray(16).also(SecureRandom()::nextBytes)
            val request = LegacyStun.bindingRequest(transaction)
            DatagramSocket().use { socket ->
                socket.soTimeout = TEST_TIMEOUT_MS
                socket.send(DatagramPacket(request, request.size, address, STUN_PORT))
                val response = ByteArray(LegacyStun.RESPONSE_SIZE)
                val packet = DatagramPacket(response, response.size)
                socket.receive(packet)
                packet.address == address &&
                    LegacyStun.isBindingResponse(response, packet.length, transaction)
            }
        }.getOrDefault(false)

    @Suppress("DEPRECATION")
    private fun openConfig() {
        startActivityForResult(
            Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "text/plain"
            },
            REQUEST_IMPORT_CONFIG,
        )
    }

    @Suppress("DEPRECATION")
    private fun createConfig() {
        startActivityForResult(
            Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "text/plain"
                putExtra(Intent.EXTRA_TITLE, "codboz-config.txt")
            },
            REQUEST_EXPORT_CONFIG,
        )
    }

    private fun importConfig(uri: android.net.Uri) {
        io.execute {
            val outcome =
                runCatching {
                    val bytes =
                        contentResolver.openInputStream(uri).use { input ->
                            requireNotNull(input) { getString(R.string.config_open_failed) }
                            val output = ByteArrayOutputStream()
                            val buffer = ByteArray(4096)
                            while (output.size() <= ConfigDocument.MAX_SIZE) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                output.write(buffer, 0, read)
                            }
                            output.toByteArray()
                        }
                    require(bytes.size <= ConfigDocument.MAX_SIZE) {
                        getString(R.string.config_too_large)
                    }
                    val content =
                        Charsets.UTF_8
                            .newDecoder()
                            .onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT)
                            .decode(ByteBuffer.wrap(bytes))
                            .toString()
                    config.replace(content)
                }
            runOnUiThread {
                if (destroyed.get()) return@runOnUiThread
                outcome.fold(
                    onSuccess = {
                        load()
                        showStatus(R.string.config_imported, false)
                    },
                    onFailure = {
                        showStatus(
                            getString(R.string.config_import_failed, it.message ?: getString(R.string.invalid_config)),
                            true,
                        )
                    },
                )
            }
        }
    }

    private fun exportConfig(uri: android.net.Uri) {
        io.execute {
            val outcome =
                runCatching {
                    contentResolver.openOutputStream(uri, "wt").use { output ->
                        requireNotNull(output) { getString(R.string.config_write_failed) }
                        output.write(config.text().toByteArray(Charsets.UTF_8))
                    }
                }
            runOnUiThread {
                if (destroyed.get()) return@runOnUiThread
                outcome.fold(
                    onSuccess = { showStatus(R.string.config_exported, false) },
                    onFailure = { showStatus(it.message ?: getString(R.string.config_write_failed), true) },
                )
            }
        }
    }

    private fun showStatus(
        stringId: Int,
        error: Boolean,
    ) = showStatus(getString(stringId), error)

    private fun showStatus(
        message: String,
        error: Boolean,
    ) {
        status.setTextColor(getColor(if (error) R.color.signal_red else R.color.signal_green))
        status.text = message
    }

    private fun animateEntrance() {
        val content = findViewById<View>(R.id.settings_content)
        content.alpha = 0f
        content.translationY = 16f * resources.displayMetrics.density
        content
            .animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(280)
            .start()
    }

    private data class ConnectionResult(
        val address: Inet4Address? = null,
        val tcp: Boolean = false,
        val udp: Boolean = false,
        val error: String? = null,
    )

    companion object {
        private const val REQUEST_IMPORT_CONFIG = 51
        private const val REQUEST_EXPORT_CONFIG = 52
        private const val REQUEST_RECORD_AUDIO = 53
        private const val GAME_PORT = 3074
        private const val STUN_PORT = 3478
        private const val TEST_TIMEOUT_MS = 2_000
    }
}
