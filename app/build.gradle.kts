import org.gradle.api.file.DirectoryProperty
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.Sync

plugins {
    alias(libs.plugins.android.application)
}

abstract class PrepareProprietaryTestAssets : Sync() {
    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty
}

abstract class PrepareLegalNotices : Sync() {
    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty
}

fun buildConfigString(value: String): String =
    buildString {
        append('"')
        value.forEach { character ->
            append(
                when (character) {
                    '\\' -> "\\\\"
                    '"' -> "\\\""
                    '\n' -> "\\n"
                    '\r' -> "\\r"
                    '\t' -> "\\t"
                    else -> character
                },
            )
        }
        append('"')
    }

val defaultConfigText = file("../config/default-config.txt").readText()
val releaseSigningValues =
    listOf(
        "CODBOZ_RELEASE_KEYSTORE",
        "CODBOZ_RELEASE_STORE_PASSWORD",
        "CODBOZ_RELEASE_KEY_ALIAS",
        "CODBOZ_RELEASE_KEY_PASSWORD",
    ).associateWith { providers.environmentVariable(it).orNull }
val releaseSigningConfigured = releaseSigningValues.values.all { !it.isNullOrBlank() }
check(releaseSigningValues.values.none { !it.isNullOrBlank() } || releaseSigningConfigured) {
    "Release signing requires all CODBOZ_RELEASE_* environment variables"
}
val proprietaryTestAssets = layout.buildDirectory.dir("generated/proprietary-test-assets")
val proprietaryTestApk =
    providers.environmentVariable("CODBOZ_TEST_APK").orNull?.let { path ->
        file(path).also { source ->
            require(source.isFile) { "CODBOZ_TEST_APK does not name a file: $path" }
        }
    }
val prepareProprietaryTestAssets =
    tasks.register<PrepareProprietaryTestAssets>("prepareProprietaryTestAssets") {
        outputDirectory.set(proprietaryTestAssets)
        into(outputDirectory)
        if (proprietaryTestApk != null) {
            from(proprietaryTestApk) {
                rename { "source-game.apk" }
            }
        }
    }
val ndkLlvmNotice =
    androidComponents.sdkComponents.ndkDirectory.map { ndkDirectory ->
        val prebuiltDirectory = ndkDirectory.asFile.resolve("toolchains/llvm/prebuilt")
        prebuiltDirectory
            .listFiles()
            ?.singleOrNull { it.isDirectory }
            ?.resolve("NOTICE")
            ?.takeIf { it.isFile }
            ?: error("NDK LLVM notice was not found under $prebuiltDirectory")
    }
val prepareLegalNotices =
    tasks.register<PrepareLegalNotices>("prepareLegalNotices") {
        outputDirectory.set(layout.buildDirectory.dir("generated/legal-notices"))
        into(outputDirectory)
        from(rootProject.file("NOTICE")) {
            rename { "project_notice.txt" }
        }
        from(rootProject.file("LICENSE")) {
            rename { "project_license.txt" }
        }
        from(ndkLlvmNotice) {
            rename { "ndk_llvm_notice.txt" }
        }
    }

android {
    namespace = "dev.producdevity.codboz"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "dev.producdevity.codboz"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("String", "DEFAULT_CONFIG", buildConfigString(defaultConfigText))

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    signingConfigs {
        if (releaseSigningConfigured) {
            create("release") {
                storeFile = file(requireNotNull(releaseSigningValues["CODBOZ_RELEASE_KEYSTORE"]))
                storePassword = releaseSigningValues["CODBOZ_RELEASE_STORE_PASSWORD"]
                keyAlias = releaseSigningValues["CODBOZ_RELEASE_KEY_ALIAS"]
                keyPassword = releaseSigningValues["CODBOZ_RELEASE_KEY_PASSWORD"]
            }
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        create("instrumentation") {
            initWith(getByName("debug"))
            applicationIdSuffix = ".testbed"
            matchingFallbacks += listOf("debug")
        }
        release {
            isMinifyEnabled = false
            if (releaseSigningConfigured) {
                signingConfig = signingConfigs.getByName("release")
            }
            ndk {
                debugSymbolLevel = "FULL"
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }

    buildFeatures {
        buildConfig = true
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
    }

    testBuildType = "instrumentation"

    sourceSets {
        getByName("main").assets.directories.add("../config")
    }
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(prepareLegalNotices) { task ->
            task.outputDirectory
        }
    }
    onVariants(selector().withBuildType("instrumentation")) { variant ->
        variant.androidTest?.sources?.assets?.addGeneratedSourceDirectory(
            prepareProprietaryTestAssets,
        ) { task ->
            task.outputDirectory
        }
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation(libs.androidx.test.ext.junit)
    androidTestImplementation(libs.androidx.test.runner)
}
