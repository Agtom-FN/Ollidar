package com.lidarscan.core.engine

/**
 * Manual "DI-lite" selector for the active [EngineBridge] — no Hilt/Dagger
 * yet, per the B1 brief. Defaults to [FakeEngineBridge] so every screen that
 * needs an engine (Capture in B4, connect flows in B2/B3) has something to
 * bind against today. When A1's JNI bridge exists, B2/B3 call [override]
 * once at app startup (or behind a build/debug flag) to swap it in; nothing
 * downstream needs to change because everything is coded against
 * [EngineBridge].
 *
 * Not thread-safety-hardened beyond `@Volatile` — this is a single value
 * set once near app start, not a general service locator.
 */
object EngineBridgeProvider {

    @Volatile
    private var instance: EngineBridge? = null

    /** Returns the current bridge, lazily creating the [FakeEngineBridge] default on first use. */
    fun get(): EngineBridge = instance ?: synchronized(this) {
        instance ?: FakeEngineBridge().also { instance = it }
    }

    /** Swaps in a different [EngineBridge] (the real JNI bridge, or a test double). */
    fun override(bridge: EngineBridge) {
        instance = bridge
    }
}
