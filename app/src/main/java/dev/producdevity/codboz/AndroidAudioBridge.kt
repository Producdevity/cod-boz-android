package dev.producdevity.codboz

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaPlayer
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.io.File
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

class AndroidAudioBridge(
    context: Context,
) {
    private val context = context.applicationContext
    private val audioManager = context.getSystemService(AudioManager::class.java)
    private val thread = HandlerThread("codboz-audio").apply { start() }
    private val handler = Handler(thread.looper)
    private val status = AtomicInteger(STOPPED)
    private val closed = AtomicBoolean(false)
    private var player: MediaPlayer? = null
    private var volume = 256
    private var playsRemaining = 0
    private var resumeAfterInterruption = false
    private var resumeAfterHostPause = false
    private var focusHeld = false
    private val sounds = arrayOfNulls<AudioTrack>(SOUND_CHANNELS)
    private val soundVolumes = IntArray(SOUND_CHANNELS) { 256 }
    private val completedSounds = AtomicInteger(0)
    private val resumeSoundsAfterInterruption = BooleanArray(SOUND_CHANNELS)
    private val resumeSoundsAfterHostPause = BooleanArray(SOUND_CHANNELS)
    private val activeSounds = AtomicInteger(0)
    private val voiceRunning = AtomicBoolean(false)
    private var resumeVoiceAfterInterruption = false
    private var resumeVoiceAfterHostPause = false
    private val voiceInput = ArrayDeque<ShortArray>()
    private val voiceOutputQueue = ArrayBlockingQueue<ShortArray>(MAX_VOICE_OUTPUT_CHUNKS)
    private var voiceRecord: AudioRecord? = null
    private var voiceCaptureThread: Thread? = null
    private var voiceOutput: AudioTrack? = null
    private var voiceOutputThread: Thread? = null
    private val voiceOutputRate = AtomicInteger(0)
    private val voiceStartAttempts = AtomicLong(0)
    private val voiceStartSuccesses = AtomicLong(0)
    private val voiceCaptureChunks = AtomicLong(0)
    private val voiceCaptureSamples = AtomicLong(0)
    private val voiceCaptureNonSilentChunks = AtomicLong(0)
    private val voiceCaptureErrors = AtomicLong(0)
    private val voiceInputDrops = AtomicLong(0)
    private val voiceOutputQueuedChunks = AtomicLong(0)
    private val voiceOutputQueuedSamples = AtomicLong(0)
    private val voiceOutputWrittenChunks = AtomicLong(0)
    private val voiceOutputWrittenSamples = AtomicLong(0)
    private val voiceOutputPartialWrites = AtomicLong(0)
    private val voiceOutputErrors = AtomicLong(0)
    private val voiceOutputDrops = AtomicLong(0)

    private val focusListener =
        AudioManager.OnAudioFocusChangeListener { change ->
            when (change) {
                AudioManager.AUDIOFOCUS_GAIN -> {
                    focusHeld = true
                    if (resumeAfterInterruption) {
                        resumeAfterInterruption = false
                        resumePlayer()
                    }
                    resumeMarkedSounds(resumeSoundsAfterInterruption)
                    if (resumeVoiceAfterInterruption) {
                        resumeVoiceAfterInterruption = false
                        startVoiceInternal()
                    }
                }

                AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
                AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK,
                -> {
                    resumeAfterInterruption = status.get() == PLAYING
                    pausePlayer()
                    pausePlayingSounds(resumeSoundsAfterInterruption)
                    resumeVoiceAfterInterruption = voiceRunning.get()
                    stopVoiceInternal(abandonFocus = false)
                }

                AudioManager.AUDIOFOCUS_LOSS -> {
                    focusHeld = false
                    resumeAfterInterruption = false
                    resumeAfterHostPause = false
                    resumeVoiceAfterInterruption = false
                    resumeVoiceAfterHostPause = false
                    resumeSoundsAfterInterruption.fill(false)
                    resumeSoundsAfterHostPause.fill(false)
                    stopPlayer()
                    releaseAllSounds()
                    stopVoiceInternal(abandonFocus = false)
                }
            }
        }

    private val focusRequest by lazy {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioFocusRequest
                .Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(AUDIO_ATTRIBUTES)
                .setOnAudioFocusChangeListener(focusListener, handler)
                .build()
        } else {
            null
        }
    }

    fun isAvailable(): Boolean = !closed.get()

    @Suppress("DEPRECATION")
    fun outputSampleRate(): Int = AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC)

    fun playMusic(
        path: String,
        repeat: Int,
    ): Boolean =
        call(false) {
            val file = File(path)
            if (!file.isFile || !requestAudioFocus()) return@call false
            stopPlayer(abandonFocus = false)
            runCatching {
                player =
                    MediaPlayer().apply {
                        setAudioAttributes(AUDIO_ATTRIBUTES)
                        setDataSource(file.absolutePath)
                        isLooping = repeat == 0
                        setVolume(volume / 256f, volume / 256f)
                        setOnCompletionListener { onTrackCompleted() }
                        setOnErrorListener { _, _, _ ->
                            stopPlayer()
                            true
                        }
                        prepare()
                        start()
                    }
                playsRemaining = repeat.coerceAtLeast(0)
                status.set(PLAYING)
                true
            }.getOrElse { error ->
                Log.e(TAG, "Music playback failed for $path", error)
                stopPlayer()
                false
            }
        }

    fun stopMusic() {
        call(Unit) { stopPlayer() }
    }

    fun pauseMusic() {
        call(Unit) { pausePlayer() }
    }

    fun resumeMusic() {
        call(Unit) { resumePlayer() }
    }

    fun musicStatus(): Int = status.get()

    fun setMusicVolume(value: Int) {
        call(Unit) {
            volume = value.coerceIn(0, 256)
            val level = volume / 256f
            player?.setVolume(level, level)
        }
    }

    fun playSound(
        channel: Int,
        samples: ShortArray,
        sampleRate: Int,
        volume: Int,
    ): Boolean =
        call(false) {
            if (channel !in sounds.indices || samples.isEmpty()) {
                return@call false
            }
            releaseSound(channel, abandonFocus = false)
            if (!requestAudioFocus()) {
                Log.e(TAG, "Audio focus denied for sound channel $channel")
                return@call false
            }
            runCatching {
                val track =
                    AudioTrack
                        .Builder()
                        .setAudioAttributes(SOUND_ATTRIBUTES)
                        .setAudioFormat(
                            AudioFormat
                                .Builder()
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .setSampleRate(sampleRate.coerceIn(MIN_SAMPLE_RATE, MAX_SAMPLE_RATE))
                                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                                .build(),
                        ).setBufferSizeInBytes(samples.size * Short.SIZE_BYTES)
                        .setTransferMode(AudioTrack.MODE_STATIC)
                        .build()
                val written = track.write(samples, 0, samples.size, AudioTrack.WRITE_BLOCKING)
                if (track.state == AudioTrack.STATE_UNINITIALIZED || written != samples.size) {
                    Log.e(
                        TAG,
                        "PCM upload failed for sound channel $channel: " +
                            "state=${track.state} written=$written expected=${samples.size}",
                    )
                    track.release()
                    return@runCatching false
                }
                soundVolumes[channel] = volume.coerceIn(0, 256)
                track.setVolume(soundVolumes[channel] / 256f)
                track.notificationMarkerPosition = samples.size
                track.setPlaybackPositionUpdateListener(
                    object : AudioTrack.OnPlaybackPositionUpdateListener {
                        override fun onMarkerReached(completed: AudioTrack) {
                            if (sounds[channel] === completed) markSoundCompleted(channel, completed)
                        }

                        override fun onPeriodicNotification(track: AudioTrack) = Unit
                    },
                    handler,
                )
                sounds[channel] = track
                activeSounds.getAndUpdate { it or (1 shl channel) }
                track.play()
                true
            }.getOrElse { error ->
                Log.e(TAG, "PCM playback failed for sound channel $channel", error)
                releaseSound(channel)
                false
            }
        }

    fun stopSound(channel: Int) {
        call(Unit) {
            if (channel !in sounds.indices) return@call
            if (completedSounds.get() and (1 shl channel) == 0) releaseSound(channel)
        }
    }

    fun pauseSound(channel: Int) {
        call(Unit) {
            if (activeSounds.get() and (1 shl channel) != 0) sounds.getOrNull(channel)?.pause()
        }
    }

    fun resumeSound(channel: Int) {
        call(Unit) {
            if (activeSounds.get() and (1 shl channel) != 0) {
                sounds.getOrNull(channel)?.let { if (requestAudioFocus()) it.play() }
            }
        }
    }

    fun setSoundVolume(
        channel: Int,
        value: Int,
    ) {
        call(Unit) {
            if (channel in sounds.indices) {
                soundVolumes[channel] = value.coerceIn(0, 256)
                sounds[channel]?.setVolume(soundVolumes[channel] / 256f)
            }
        }
    }

    fun activeSoundMask(): Int = activeSounds.get()

    fun isVoiceAvailable(): Boolean =
        !closed.get() &&
            context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED &&
            AudioRecord.getMinBufferSize(
                VOICE_INPUT_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
            ) > 0

    fun startVoice(): Boolean = call(false) { startVoiceInternal() }

    fun stopVoice() {
        call(Unit) { stopVoiceInternal() }
    }

    fun takeVoiceInput(): ShortArray? = synchronized(voiceInput) { voiceInput.removeFirstOrNull() }

    fun voiceInputSampleRate(): Int = VOICE_INPUT_RATE

    fun queueVoiceOutput(
        samples: ShortArray,
        sampleRate: Int,
    ): Boolean {
        if (!voiceRunning.get() || samples.isEmpty() || sampleRate != voiceOutputRate.get()) {
            voiceOutputErrors.incrementAndGet()
            return false
        }
        if (!voiceOutputQueue.offer(samples)) {
            if (voiceOutputQueue.poll() != null) voiceOutputDrops.incrementAndGet()
            if (!voiceOutputQueue.offer(samples)) {
                voiceOutputDrops.incrementAndGet()
                return false
            }
        }
        voiceOutputQueuedChunks.incrementAndGet()
        voiceOutputQueuedSamples.addAndGet(samples.size.toLong())
        return true
    }

    fun voiceDiagnostics(): String =
        "running=${if (voiceRunning.get()) 1 else 0}" +
            " start=${voiceStartSuccesses.get()}/${voiceStartAttempts.get()}" +
            " capture=${voiceCaptureChunks.get()}/${voiceCaptureSamples.get()}" +
            " capture_nonzero=${voiceCaptureNonSilentChunks.get()}" +
            " capture_errors=${voiceCaptureErrors.get()}" +
            " input_drops=${voiceInputDrops.get()}" +
            " input_queue=${synchronized(voiceInput) { voiceInput.size }}" +
            " output_queue=${voiceOutputQueue.size}" +
            " output_enqueued=${voiceOutputQueuedChunks.get()}/${voiceOutputQueuedSamples.get()}" +
            " output_written=${voiceOutputWrittenChunks.get()}/${voiceOutputWrittenSamples.get()}" +
            " output_partial=${voiceOutputPartialWrites.get()}" +
            " output_errors=${voiceOutputErrors.get()}" +
            " output_drops=${voiceOutputDrops.get()}" +
            " output_rate=${voiceOutputRate.get()}"

    fun onHostPause() {
        dispatch {
            resumeAfterHostPause = status.get() == PLAYING || resumeAfterInterruption
            resumeAfterInterruption = false
            pausePlayer()
            pausePlayingSounds(resumeSoundsAfterHostPause)
            resumeSoundsAfterInterruption.forEachIndexed { channel, shouldResume ->
                if (shouldResume) resumeSoundsAfterHostPause[channel] = true
            }
            resumeSoundsAfterInterruption.fill(false)
            resumeVoiceAfterHostPause = voiceRunning.get() || resumeVoiceAfterInterruption
            resumeVoiceAfterInterruption = false
            stopVoiceInternal(abandonFocus = false)
            abandonAudioFocus()
        }
    }

    fun onHostResume() {
        dispatch {
            val shouldResumeSounds = resumeSoundsAfterHostPause.any { it }
            if ((resumeAfterHostPause || shouldResumeSounds || resumeVoiceAfterHostPause) &&
                !requestAudioFocus()
            ) {
                return@dispatch
            }
            if (resumeAfterHostPause) {
                resumeAfterHostPause = false
                resumePlayer()
            }
            resumeMarkedSounds(resumeSoundsAfterHostPause)
            if (resumeVoiceAfterHostPause) {
                resumeVoiceAfterHostPause = false
                startVoiceInternal()
            }
        }
    }

    fun shutdown() {
        if (!closed.compareAndSet(false, true)) return
        val latch = CountDownLatch(1)
        val posted =
            handler.post {
                try {
                    stopVoiceInternal()
                    resumeVoiceAfterInterruption = false
                    resumeVoiceAfterHostPause = false
                    stopPlayer()
                    releaseAllSounds()
                } catch (error: Throwable) {
                    Log.e(TAG, "Audio shutdown failed", error)
                } finally {
                    latch.countDown()
                }
            }
        if (posted) {
            latch.await(CALL_TIMEOUT_SECONDS, TimeUnit.SECONDS)
        }
        thread.quitSafely()
        runCatching { thread.join(CALL_TIMEOUT_SECONDS * 1_000L) }
    }

    private fun onTrackCompleted() {
        if (playsRemaining > 1) {
            playsRemaining--
            player?.seekTo(0)
            player?.start()
        } else {
            stopPlayer()
        }
    }

    @Suppress("MissingPermission")
    private fun startVoiceInternal(): Boolean {
        if (voiceRunning.get()) return true
        if (!isVoiceAvailable() || !requestAudioFocus()) return false
        voiceStartAttempts.incrementAndGet()

        val inputBufferBytes =
            AudioRecord
                .getMinBufferSize(
                    VOICE_INPUT_RATE,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                ).coerceAtLeast(VOICE_INPUT_SAMPLES * Short.SIZE_BYTES * 2)
        val outputRate = outputSampleRate().coerceIn(MIN_SAMPLE_RATE, MAX_SAMPLE_RATE)
        val outputBufferBytes =
            AudioTrack
                .getMinBufferSize(
                    outputRate,
                    AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                ).coerceAtLeast(outputRate / 25 * Short.SIZE_BYTES)

        return runCatching {
            val record =
                AudioRecord
                    .Builder()
                    .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
                    .setAudioFormat(
                        AudioFormat
                            .Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(VOICE_INPUT_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_IN_MONO)
                            .build(),
                    ).setBufferSizeInBytes(inputBufferBytes)
                    .build()
            val output =
                AudioTrack
                    .Builder()
                    .setAudioAttributes(VOICE_ATTRIBUTES)
                    .setAudioFormat(
                        AudioFormat
                            .Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setSampleRate(outputRate)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .build(),
                    ).setBufferSizeInBytes(outputBufferBytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build()
            if (record.state != AudioRecord.STATE_INITIALIZED ||
                output.state != AudioTrack.STATE_INITIALIZED
            ) {
                record.release()
                output.release()
                return@runCatching false
            }
            synchronized(voiceInput) { voiceInput.clear() }
            voiceOutputQueue.clear()
            voiceRecord = record
            voiceOutput = output
            record.startRecording()
            output.play()
            if (record.recordingState != AudioRecord.RECORDSTATE_RECORDING ||
                output.playState != AudioTrack.PLAYSTATE_PLAYING
            ) {
                error("Voice audio did not enter the running state")
            }
            voiceOutputRate.set(outputRate)
            voiceRunning.set(true)
            voiceCaptureThread =
                Thread({ captureVoice(record) }, "codboz-voice-capture").apply { start() }
            voiceOutputThread =
                Thread({ playVoice(output) }, "codboz-voice-output").apply { start() }
            voiceStartSuccesses.incrementAndGet()
            true
        }.getOrElse { error ->
            Log.e(TAG, "Voice audio could not start", error)
            stopVoiceInternal()
            false
        }
    }

    private fun captureVoice(record: AudioRecord) {
        val samples = ShortArray(VOICE_INPUT_SAMPLES)
        while (voiceRunning.get()) {
            val count = record.read(samples, 0, samples.size, AudioRecord.READ_BLOCKING)
            if (count <= 0) {
                if (voiceRunning.get()) voiceCaptureErrors.incrementAndGet()
                continue
            }
            if (!voiceRunning.get()) continue
            val chunk = samples.copyOf(count)
            voiceCaptureChunks.incrementAndGet()
            voiceCaptureSamples.addAndGet(count.toLong())
            if (chunk.any { it.toInt() != 0 }) voiceCaptureNonSilentChunks.incrementAndGet()
            synchronized(voiceInput) {
                while (voiceInput.size >= MAX_VOICE_INPUT_CHUNKS) {
                    voiceInput.removeFirst()
                    voiceInputDrops.incrementAndGet()
                }
                voiceInput.addLast(chunk)
            }
        }
    }

    private fun playVoice(output: AudioTrack) {
        while (voiceRunning.get() || voiceOutputQueue.isNotEmpty()) {
            val samples =
                try {
                    voiceOutputQueue.poll(VOICE_OUTPUT_POLL_MS, TimeUnit.MILLISECONDS)
                } catch (_: InterruptedException) {
                    if (!voiceRunning.get()) break
                    continue
                } ?: continue
            var offset = 0
            while (offset < samples.size && voiceRunning.get()) {
                val written =
                    output.write(
                        samples,
                        offset,
                        samples.size - offset,
                        AudioTrack.WRITE_BLOCKING,
                    )
                if (written <= 0) {
                    voiceOutputErrors.incrementAndGet()
                    break
                }
                offset += written
            }
            voiceOutputWrittenSamples.addAndGet(offset.toLong())
            if (offset == samples.size) {
                voiceOutputWrittenChunks.incrementAndGet()
            } else {
                voiceOutputPartialWrites.incrementAndGet()
            }
        }
    }

    private fun stopVoiceInternal(abandonFocus: Boolean = true) {
        voiceRunning.set(false)
        voiceRecord?.let { record ->
            runCatching { record.stop() }
        }
        voiceCaptureThread?.let { thread ->
            runCatching { thread.join(VOICE_THREAD_JOIN_MS) }
        }
        voiceCaptureThread = null
        voiceRecord?.let { record -> runCatching { record.release() } }
        voiceRecord = null
        voiceOutput?.let { output ->
            runCatching { output.pause() }
            runCatching { output.flush() }
            runCatching { output.stop() }
        }
        voiceOutputThread?.interrupt()
        voiceOutputThread?.let { thread ->
            runCatching { thread.join(VOICE_THREAD_JOIN_MS) }
        }
        voiceOutputThread = null
        voiceOutput?.let { output -> runCatching { output.release() } }
        voiceOutput = null
        voiceOutputRate.set(0)
        voiceOutputQueue.clear()
        synchronized(voiceInput) { voiceInput.clear() }
        if (abandonFocus) abandonAudioFocusIfIdle()
    }

    private fun pausePlayer() {
        val active = player ?: return
        if (status.get() == PLAYING && active.isPlaying) {
            active.pause()
            status.set(PAUSED)
        }
    }

    private fun resumePlayer() {
        val active = player ?: return
        if (status.get() == PAUSED && requestAudioFocus()) {
            active.start()
            status.set(PLAYING)
        }
    }

    private fun stopPlayer(abandonFocus: Boolean = true) {
        player?.let { active ->
            runCatching { active.stop() }
            runCatching { active.reset() }
            runCatching { active.release() }
        }
        player = null
        status.set(STOPPED)
        playsRemaining = 0
        if (abandonFocus) abandonAudioFocusIfIdle()
    }

    private fun pausePlayingSounds(marked: BooleanArray) {
        sounds.forEachIndexed { channel, track ->
            marked[channel] =
                activeSounds.get() and (1 shl channel) != 0 &&
                track?.playState == AudioTrack.PLAYSTATE_PLAYING
            if (marked[channel]) track?.pause()
        }
    }

    private fun resumeMarkedSounds(marked: BooleanArray) {
        marked.forEachIndexed { channel, shouldResume ->
            if (shouldResume) {
                marked[channel] = false
                sounds[channel]?.play()
            }
        }
    }

    private fun releaseAllSounds() {
        sounds.indices.forEach { releaseSound(it, abandonFocus = false) }
        abandonAudioFocusIfIdle()
    }

    private fun markSoundCompleted(
        channel: Int,
        track: AudioTrack,
    ) {
        track.setPlaybackPositionUpdateListener(null)
        activeSounds.getAndUpdate { it and (1 shl channel).inv() }
        completedSounds.getAndUpdate { it or (1 shl channel) }
        resumeSoundsAfterInterruption[channel] = false
        resumeSoundsAfterHostPause[channel] = false
        abandonAudioFocusIfIdle()
        handler.postDelayed(
            {
                if (
                    sounds[channel] === track &&
                    completedSounds.get() and (1 shl channel) != 0
                ) {
                    releaseSound(channel)
                }
            },
            SOUND_TAIL_RELEASE_DELAY_MS,
        )
    }

    private fun releaseSound(
        channel: Int,
        abandonFocus: Boolean = true,
    ) {
        if (channel !in sounds.indices) return
        sounds[channel]?.let { track ->
            runCatching { track.stop() }
            runCatching { track.flush() }
            runCatching { track.release() }
        }
        sounds[channel] = null
        completedSounds.getAndUpdate { it and (1 shl channel).inv() }
        resumeSoundsAfterInterruption[channel] = false
        resumeSoundsAfterHostPause[channel] = false
        activeSounds.getAndUpdate { it and (1 shl channel).inv() }
        if (abandonFocus) abandonAudioFocusIfIdle()
    }

    private fun abandonAudioFocusIfIdle() {
        if (player == null && activeSounds.get() == 0 && !voiceRunning.get()) abandonAudioFocus()
    }

    private fun requestAudioFocus(): Boolean {
        if (focusHeld) return true
        val result =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                audioManager.requestAudioFocus(requireNotNull(focusRequest))
            } else {
                @Suppress("DEPRECATION")
                audioManager.requestAudioFocus(
                    focusListener,
                    AudioManager.STREAM_MUSIC,
                    AudioManager.AUDIOFOCUS_GAIN,
                )
            }
        focusHeld = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        return focusHeld
    }

    private fun abandonAudioFocus() {
        if (!focusHeld) return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let(audioManager::abandonAudioFocusRequest)
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(focusListener)
        }
        focusHeld = false
    }

    private fun <T> call(
        fallback: T,
        block: () -> T,
    ): T {
        if (closed.get() && !handler.looper.isCurrentThread) return fallback
        if (handler.looper.isCurrentThread) {
            return runCatching(block).getOrElse { error ->
                Log.e(TAG, "Audio operation failed", error)
                fallback
            }
        }
        val latch = CountDownLatch(1)
        var result = fallback
        if (!handler.post {
                try {
                    result = block()
                } catch (error: Throwable) {
                    Log.e(TAG, "Audio operation failed", error)
                } finally {
                    latch.countDown()
                }
            }
        ) {
            return fallback
        }
        return if (latch.await(CALL_TIMEOUT_SECONDS, TimeUnit.SECONDS)) result else fallback
    }

    private fun dispatch(block: () -> Unit) {
        if (closed.get()) return
        if (handler.looper.isCurrentThread) {
            runCatching(block).onFailure { error -> Log.e(TAG, "Audio operation failed", error) }
            return
        }
        handler.post {
            runCatching(block).onFailure { error -> Log.e(TAG, "Audio operation failed", error) }
        }
    }

    companion object {
        private const val STOPPED = 0
        private const val PLAYING = 1
        private const val PAUSED = 2
        private const val CALL_TIMEOUT_SECONDS = 5L
        private const val SOUND_TAIL_RELEASE_DELAY_MS = 250L
        private const val SOUND_CHANNELS = 24
        private const val VOICE_INPUT_RATE = 8_000
        private const val VOICE_INPUT_SAMPLES = 512
        private const val MAX_VOICE_INPUT_CHUNKS = 16
        private const val MAX_VOICE_OUTPUT_CHUNKS = 8
        private const val VOICE_OUTPUT_POLL_MS = 100L
        private const val VOICE_THREAD_JOIN_MS = 1_000L
        private const val MIN_SAMPLE_RATE = 4_000
        private const val MAX_SAMPLE_RATE = 192_000
        private const val TAG = "codboz-audio"

        private val AUDIO_ATTRIBUTES =
            AudioAttributes
                .Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
        private val SOUND_ATTRIBUTES =
            AudioAttributes
                .Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build()
        private val VOICE_ATTRIBUTES =
            AudioAttributes
                .Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
    }
}
