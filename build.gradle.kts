plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.spotless)
}

spotless {
    kotlin {
        target("app/src/**/*.kt")
        ktlint()
    }
    kotlinGradle {
        target("*.gradle.kts", "app/*.gradle.kts")
        ktlint()
    }
}
