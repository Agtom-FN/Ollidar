// :core — project/data layer. Plain Kotlin/JVM module, deliberately free of
// any Android or Compose/UI dependency: its logic runs and is unit-tested on
// a bare JVM (no emulator, no AGP, no Robolectric) and stays reusable if a
// non-Android client ever needs the same project store / engine contracts.
plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(libs.plugins.kotlin.serialization)
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.core)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}
