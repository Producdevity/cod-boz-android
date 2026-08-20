package dev.producdevity.codboz

import android.app.Application

class CodBozApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        CrashDiagnostics.install(this)
        CrashDiagnostics.setProcessState(this, "application-start")
    }
}
