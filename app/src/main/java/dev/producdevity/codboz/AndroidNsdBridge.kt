package dev.producdevity.codboz

import android.annotation.SuppressLint
import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.SystemClock
import android.util.Log
import java.net.Inet4Address
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class AndroidNsdBridge(
    context: Context,
) {
    private val nsd = context.getSystemService(NsdManager::class.java)
    private val multicastLock =
        context
            .getSystemService(WifiManager::class.java)
            .createMulticastLock("codboz-local-discovery")
            .apply { setReferenceCounted(false) }
    private val searches = ConcurrentHashMap<Int, Search>()
    private val publications = ConcurrentHashMap<Int, Publication>()
    private val handler = Handler(context.mainLooper)
    private val callbackExecutor = Executor { command -> handler.post(command) }
    private val activeOperations = AtomicInteger(0)
    private val closed = AtomicBoolean(false)

    fun startSearch(
        handle: Int,
        serviceType: String,
        domain: String,
    ): Boolean {
        if (closed.get() || serviceType.isBlank() || handle == 0 || searches.containsKey(handle)) {
            return false
        }
        val androidType = androidServiceType(serviceType, domain) ?: return false
        val listener = DiscoveryListener(handle)
        searches[handle] = Search(listener)
        retainMulticast()
        return runCatching {
            nsd.discoverServices(androidType, NsdManager.PROTOCOL_DNS_SD, listener)
            true
        }.getOrElse { error ->
            searches.remove(handle)
            releaseMulticast()
            Log.e(TAG, "Could not start local discovery", error)
            false
        }
    }

    fun stopSearch(handle: Int) {
        val search = searches.remove(handle) ?: return
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            search.serviceCallbacks.values.forEach { listener ->
                runCatching { nsd.unregisterServiceInfoCallback(listener) }
            }
        }
        search.serviceCallbacks.clear()
        search.resolvers.values.forEach { listener ->
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                runCatching { nsd.stopServiceResolution(listener) }
            }
        }
        search.resolvers.clear()
        runCatching { nsd.stopServiceDiscovery(search.listener) }
        releaseMulticast()
    }

    fun publish(
        handle: Int,
        port: Int,
        name: String,
        serviceType: String,
        domain: String,
        txtRecords: Array<String>,
    ): Boolean {
        if (closed.get() || handle == 0 || port !in 1..65535 || name.isBlank() ||
            publications.containsKey(handle)
        ) {
            return false
        }
        val androidType = androidServiceType(serviceType, domain) ?: return false
        val attributes = attributes(txtRecords) ?: return false
        val service =
            NsdServiceInfo().apply {
                serviceName = name
                this.serviceType = androidType
                this.port = port
                attributes.forEach { (key, value) -> setAttribute(key, value) }
            }
        val listener = RegistrationListener(handle)
        publications[handle] = Publication(service, listener, txtRecords.toList())
        retainMulticast()
        return runCatching {
            nsd.registerService(service, NsdManager.PROTOCOL_DNS_SD, listener)
            true
        }.getOrElse { error ->
            publications.remove(handle)
            releaseMulticast()
            Log.e(TAG, "Could not publish local game", error)
            false
        }
    }

    fun update(
        handle: Int,
        txtRecords: Array<String>,
    ): Boolean {
        val publication = publications[handle] ?: return false
        if (attributes(txtRecords) == null) return false
        val updated = txtRecords.toList()
        synchronized(publication) {
            if (publication.desiredTxtRecords == updated) return true
            publication.desiredTxtRecords = updated
            if (!publication.registered || publication.replacing || publication.updateScheduled) {
                return true
            }
            publication.updateScheduled = true
        }
        handler.postAtTime(
            {
                synchronized(publication) { publication.updateScheduled = false }
                maybeReplacePublication(handle, publication)
            },
            publication,
            SystemClock.uptimeMillis(),
        )
        return true
    }

    fun unpublish(handle: Int) {
        val publication = publications.remove(handle) ?: return
        handler.removeCallbacksAndMessages(publication)
        runCatching { nsd.unregisterService(publication.listener) }
        releaseMulticast()
    }

    fun shutdown() {
        if (!closed.compareAndSet(false, true)) return
        searches.keys.toList().forEach(::stopSearch)
        publications.keys.toList().forEach(::unpublish)
        handler.removeCallbacksAndMessages(null)
        activeOperations.set(0)
        if (multicastLock.isHeld) multicastLock.release()
    }

    private fun maybeReplacePublication(
        handle: Int,
        publication: Publication,
    ) {
        if (closed.get() || publications[handle] !== publication) return
        synchronized(publication) {
            if (!publication.registered || publication.replacing ||
                publication.desiredTxtRecords == publication.advertisedTxtRecords
            ) {
                return
            }
            val dueAt = publication.lastRegistrationMs + MIN_UPDATE_INTERVAL_MS
            if (SystemClock.uptimeMillis() < dueAt) {
                if (!publication.updateScheduled) {
                    publication.updateScheduled = true
                    handler.postAtTime(
                        {
                            synchronized(publication) { publication.updateScheduled = false }
                            maybeReplacePublication(handle, publication)
                        },
                        publication,
                        dueAt,
                    )
                }
                return
            }
            publication.replacing = true
        }
        runCatching { nsd.unregisterService(publication.listener) }.onFailure {
            synchronized(publication) { publication.replacing = false }
            nativeOperationFailed(handle, true)
        }
    }

    private inner class DiscoveryListener(
        private val handle: Int,
    ) : NsdManager.DiscoveryListener {
        override fun onDiscoveryStarted(serviceType: String) = Unit

        override fun onDiscoveryStopped(serviceType: String) = Unit

        override fun onServiceFound(serviceInfo: NsdServiceInfo) {
            val search = searches[handle] ?: return
            val key = serviceKey(serviceInfo)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                val listener = ServiceInfoListener(handle, key)
                if (search.serviceCallbacks.putIfAbsent(key, listener) != null) return
                runCatching {
                    nsd.registerServiceInfoCallback(serviceInfo, callbackExecutor, listener)
                }.onFailure {
                    search.serviceCallbacks.remove(key, listener)
                }
            } else {
                val listener = ResolveListener(handle, key)
                if (search.resolvers.putIfAbsent(key, listener) != null) return
                @Suppress("DEPRECATION")
                runCatching { nsd.resolveService(serviceInfo, listener) }.onFailure {
                    search.resolvers.remove(key, listener)
                }
            }
        }

        override fun onServiceLost(serviceInfo: NsdServiceInfo) {
            val key = serviceKey(serviceInfo)
            val search = searches[handle] ?: return
            search.resolvers.remove(key)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                search.serviceCallbacks.remove(key)?.let { listener ->
                    runCatching { nsd.unregisterServiceInfoCallback(listener) }
                }
            }
            reportServiceLost(handle, key)
        }

        override fun onStartDiscoveryFailed(
            serviceType: String,
            errorCode: Int,
        ) {
            searches.remove(handle)?.let {
                releaseMulticast()
                nativeOperationFailed(handle, false)
            }
        }

        override fun onStopDiscoveryFailed(
            serviceType: String,
            errorCode: Int,
        ) = Unit
    }

    private inner class ResolveListener(
        private val handle: Int,
        private val key: String,
    ) : NsdManager.ResolveListener {
        override fun onResolveFailed(
            serviceInfo: NsdServiceInfo,
            errorCode: Int,
        ) {
            searches[handle]?.resolvers?.remove(key)
        }

        override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
            searches[handle]?.resolvers?.remove(key)
            reportService(handle, key, serviceInfo)
        }
    }

    @SuppressLint("NewApi")
    private inner class ServiceInfoListener(
        private val handle: Int,
        private val key: String,
    ) : NsdManager.ServiceInfoCallback {
        override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) {
            searches[handle]?.serviceCallbacks?.remove(key, this)
        }

        override fun onServiceUpdated(serviceInfo: NsdServiceInfo) {
            reportService(handle, key, serviceInfo)
        }

        override fun onServiceLost() {
            reportServiceLost(handle, key)
        }

        override fun onServiceInfoCallbackUnregistered() {
            searches[handle]?.serviceCallbacks?.remove(key, this)
        }
    }

    private fun reportService(
        handle: Int,
        key: String,
        serviceInfo: NsdServiceInfo,
    ) {
        val search = searches[handle] ?: return
        val ipv4 =
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                serviceInfo.hostAddresses.filterIsInstance<Inet4Address>().firstOrNull()
            } else {
                @Suppress("DEPRECATION")
                serviceInfo.host as? Inet4Address
            } ?: return
        val txt =
            serviceInfo.attributes
                .entries
                .sortedBy { it.key }
                .map { (name, value) ->
                    val decoded = String(value, StandardCharsets.UTF_8)
                    if (decoded.isEmpty()) name else "$name=$decoded"
                }.toTypedArray()
        search.reportedServices += key
        nativeServiceFound(
            handle,
            key,
            serviceInfo.serviceName,
            normalizedServiceType(serviceInfo.serviceType),
            "local",
            ipv4.hostAddress.orEmpty(),
            serviceInfo.port,
            ipv4.address,
            txt,
        )
    }

    private fun reportServiceLost(
        handle: Int,
        key: String,
    ) {
        val search = searches[handle] ?: return
        if (search.reportedServices.remove(key)) nativeServiceLost(handle, key)
    }

    private inner class RegistrationListener(
        private val handle: Int,
    ) : NsdManager.RegistrationListener {
        override fun onRegistrationFailed(
            serviceInfo: NsdServiceInfo,
            errorCode: Int,
        ) {
            publications.remove(handle)?.let {
                handler.removeCallbacksAndMessages(it)
                releaseMulticast()
                nativeOperationFailed(handle, true)
            }
        }

        override fun onServiceRegistered(serviceInfo: NsdServiceInfo) {
            val publication = publications[handle] ?: return
            val changedWhileRegistering =
                synchronized(publication) {
                    publication.service = serviceInfo
                    publication.advertisedTxtRecords = publication.registeringTxtRecords
                    publication.registered = true
                    publication.replacing = false
                    publication.lastRegistrationMs = SystemClock.uptimeMillis()
                    publication.desiredTxtRecords != publication.advertisedTxtRecords
                }
            if (changedWhileRegistering) maybeReplacePublication(handle, publication)
        }

        override fun onServiceUnregistered(serviceInfo: NsdServiceInfo) {
            val publication = publications[handle] ?: return
            val replacement =
                synchronized(publication) {
                    if (!publication.replacing) return
                    val records = publication.desiredTxtRecords
                    val values = attributes(records.toTypedArray()) ?: return
                    publication.registeringTxtRecords = records
                    NsdServiceInfo()
                        .apply {
                            serviceName = serviceInfo.serviceName
                            serviceType = serviceInfo.serviceType
                            port = serviceInfo.port
                            values.forEach { (key, value) -> setAttribute(key, value) }
                        }.also { publication.service = it }
                }
            runCatching {
                nsd.registerService(
                    replacement,
                    NsdManager.PROTOCOL_DNS_SD,
                    publication.listener,
                )
            }.onFailure {
                publications.remove(handle)?.let {
                    handler.removeCallbacksAndMessages(it)
                    releaseMulticast()
                    nativeOperationFailed(handle, true)
                }
            }
        }

        override fun onUnregistrationFailed(
            serviceInfo: NsdServiceInfo,
            errorCode: Int,
        ) {
            publications[handle]?.let { publication ->
                synchronized(publication) { publication.replacing = false }
            }
            nativeOperationFailed(handle, true)
        }
    }

    private fun retainMulticast() {
        if (activeOperations.incrementAndGet() == 1 && !multicastLock.isHeld) {
            multicastLock.acquire()
        }
    }

    private fun releaseMulticast() {
        val remaining = activeOperations.updateAndGet { value -> (value - 1).coerceAtLeast(0) }
        if (remaining == 0 && multicastLock.isHeld) multicastLock.release()
    }

    private fun androidServiceType(
        serviceType: String,
        domain: String,
    ): String? {
        val type = serviceType.trim().trimEnd('.')
        val zone = domain.trim().trim('.').ifEmpty { "local" }
        if (!type.matches(NsdValues.SERVICE_TYPE) || !zone.equals("local", ignoreCase = true)) {
            return null
        }
        return "$type."
    }

    private fun attributes(records: Array<String>): Map<String, String>? {
        if (records.size > MAX_TXT_RECORDS) return null
        val values = linkedMapOf<String, String>()
        for (record in records) {
            if (record.toByteArray(StandardCharsets.UTF_8).size > MAX_TXT_BYTES) return null
            val equals = record.indexOf('=')
            val key = if (equals < 0) record else record.substring(0, equals)
            val value = if (equals < 0) "" else record.substring(equals + 1)
            if (!key.matches(TXT_KEY) || values.put(key, value) != null) return null
        }
        return values
    }

    private fun serviceKey(serviceInfo: NsdServiceInfo): String =
        "${serviceInfo.serviceName}.${normalizedServiceType(serviceInfo.serviceType)}.local"

    private fun normalizedServiceType(value: String): String = value.trim('.').removeSuffix(".local")

    private data class Search(
        val listener: NsdManager.DiscoveryListener,
        val resolvers: ConcurrentHashMap<String, NsdManager.ResolveListener> = ConcurrentHashMap(),
        val serviceCallbacks: ConcurrentHashMap<String, NsdManager.ServiceInfoCallback> =
            ConcurrentHashMap(),
        val reportedServices: MutableSet<String> = ConcurrentHashMap.newKeySet(),
    )

    private data class Publication(
        var service: NsdServiceInfo,
        val listener: NsdManager.RegistrationListener,
        var desiredTxtRecords: List<String>,
        var registeringTxtRecords: List<String> = desiredTxtRecords,
        var advertisedTxtRecords: List<String> = desiredTxtRecords,
        var registered: Boolean = false,
        var replacing: Boolean = false,
        var updateScheduled: Boolean = false,
        var lastRegistrationMs: Long = 0,
    )

    private external fun nativeServiceFound(
        searchHandle: Int,
        serviceKey: String,
        name: String,
        serviceType: String,
        domain: String,
        host: String,
        port: Int,
        ipv4Address: ByteArray,
        txtRecords: Array<String>,
    )

    private external fun nativeServiceLost(
        searchHandle: Int,
        serviceKey: String,
    )

    private external fun nativeOperationFailed(
        handle: Int,
        publication: Boolean,
    )

    private companion object {
        const val TAG = "codboz"
        const val MAX_TXT_RECORDS = 16
        const val MAX_TXT_BYTES = 255
        const val MIN_UPDATE_INTERVAL_MS = 1_000L
        val TXT_KEY = Regex("[A-Za-z0-9_-]{1,63}")
    }
}

internal object NsdValues {
    val SERVICE_TYPE =
        Regex("_[A-Za-z0-9_-]{1,61}[A-Za-z0-9]\\._(?:tcp|udp)", RegexOption.IGNORE_CASE)
}
