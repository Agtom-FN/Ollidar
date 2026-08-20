package com.lidarscan.app.net

import android.content.Context
import android.content.Intent
import android.os.Build
import android.provider.Settings

/**
 * Per-OEM guidance for configuring a static IP on a USB-C Ethernet adapter
 * (Tech Spec §3.1's Android row: "static-IP wizard + **per-OEM guidance**").
 *
 * ## Why this is guidance and not a setting this app writes
 *
 * There is **no public API to configure an Ethernet interface's IP**. The
 * class that does it, `android.net.EthernetManager` (and
 * `IpConfiguration`/`StaticIpConfiguration`), is `@SystemApi` and guarded by
 * `MANAGE_ETHERNET_NETWORKS`, which is `signature`-level: a normal app cannot
 * hold it on any retail build, and there is no user-grantable equivalent.
 * `WifiManager` has a public counterpart for Wi-Fi; Ethernet does not. So the
 * honest design is a deep link plus accurate instructions, not a form that
 * pretends to work.
 *
 * ## The per-OEM variance is real and is the point
 *
 * AOSP has had an "Ethernet" entry under Settings → Network & internet since
 * Android 11 (API 30), reachable through the generic
 * `Settings.ACTION_WIRELESS_SETTINGS` / `ACTION_NETWORK_OPERATOR_SETTINGS`
 * neighbourhood — but where it lands, what it is called, and whether it
 * exposes static IP at all differ per vendor, and some builds surface it only
 * once an adapter is physically attached. The strings below name the path per
 * OEM and are explicitly labelled as guidance so that a wrong menu name reads
 * as an out-of-date hint, not as a broken app.
 *
 * **What is verified and what is not**: the deep-link intents below are
 * standard, documented `Settings.ACTION_*` constants, and every launch goes
 * through [resolveActivity] first so an unresolvable one degrades to the
 * generic Settings screen instead of throwing `ActivityNotFoundException`.
 * The *menu paths* in the guidance text are from vendor documentation and
 * have **not** been walked on a physical device of each brand — no device was
 * available to this task. They are hints, and the copy says so.
 */
object StaticIpGuidance {

    /** Vendor buckets that actually differ in where Ethernet settings live. */
    enum class Oem(val label: String) {
        PIXEL_AOSP("Pixel / AOSP"),
        SAMSUNG("Samsung (One UI)"),
        XIAOMI("Xiaomi / Redmi / POCO (HyperOS, MIUI)"),
        ONEPLUS_OPPO("OnePlus / OPPO / realme (ColorOS, OxygenOS)"),
        HONOR_HUAWEI("HONOR / Huawei (MagicOS, EMUI)"),
        OTHER("Other"),
    }

    fun detectOem(): Oem {
        val brand = (Build.BRAND + " " + Build.MANUFACTURER).lowercase()
        return when {
            brand.contains("google") -> Oem.PIXEL_AOSP
            brand.contains("samsung") -> Oem.SAMSUNG
            brand.contains("xiaomi") || brand.contains("redmi") || brand.contains("poco") -> Oem.XIAOMI
            brand.contains("oneplus") || brand.contains("oppo") || brand.contains("realme") -> Oem.ONEPLUS_OPPO
            brand.contains("honor") || brand.contains("huawei") -> Oem.HONOR_HUAWEI
            else -> Oem.OTHER
        }
    }

    /**
     * The steps to show. Written as short imperative lines because they are
     * read one-handed, standing up, with a lidar in the other hand.
     *
     * @param targetHostIp AUTO-DETECT: when a Mid-360 heartbeat has been
     *   received and its persisted host IP does not match anything this
     *   phone's Ethernet interface currently holds, the wizard passes that
     *   exact address here so the last step names the **specific** value to
     *   type into Settings — the address the lidar is already configured to
     *   stream to — instead of the generic `192.168.1.5`-shaped hint the
     *   per-OEM steps otherwise give. Null (the default) leaves the steps
     *   unchanged.
     */
    fun steps(oem: Oem = detectOem(), targetHostIp: String? = null): List<String> {
        val base = rawSteps(oem)
        return if (targetHostIp.isNullOrBlank()) {
            base
        } else {
            base + "Set this phone's static IP to $targetHostIp/24 — the exact address the Mid-360 is " +
                "already configured to stream to (read from its own heartbeat broadcast, not a guess)."
        }
    }

    private fun rawSteps(oem: Oem): List<String> = when (oem) {
        Oem.PIXEL_AOSP -> listOf(
            "Plug the USB-C Ethernet adapter in first — the entry does not appear until an adapter is attached.",
            "Settings → Network & internet → Ethernet.",
            "Turn off DHCP / choose \"Static\" and enter the host IP, gateway and a /24 netmask (255.255.255.0).",
            "Leave DNS blank or 8.8.8.8 — nothing on this link resolves names.",
        )
        Oem.SAMSUNG -> listOf(
            "Plug the USB-C Ethernet adapter in first.",
            "Settings → Connections → More connection settings → Ethernet.",
            "Tap the gear beside the adapter → IP settings → Static.",
            "One UI hides Ethernet entirely on some models when no adapter is attached — if you cannot find it, check the cable and the adapter's own LED.",
        )
        Oem.XIAOMI -> listOf(
            "Plug the USB-C Ethernet adapter in first.",
            "Settings → Connection & sharing → Ethernet (some builds: Settings → Additional settings → Ethernet).",
            "Switch IP assignment to Static and enter the host IP with a 255.255.255.0 netmask.",
            "HyperOS has moved this entry more than once; if it is absent, search Settings for \"Ethernet\".",
        )
        Oem.ONEPLUS_OPPO -> listOf(
            "Plug the USB-C Ethernet adapter in first.",
            "Settings → Wi-Fi & network (or Connection & sharing) → Ethernet.",
            "IP settings → Static, then enter the host IP and a 255.255.255.0 netmask.",
        )
        Oem.HONOR_HUAWEI -> listOf(
            "Plug the USB-C Ethernet adapter in first.",
            "Settings → Mobile network / More connections → Ethernet.",
            "Some MagicOS/EMUI builds ship no Ethernet UI at all. If this is one of them, the adapter can still work with DHCP from a small switch — put a router or a DHCP-capable switch between the phone and the lidar and read the assigned address back on this screen.",
        )
        Oem.OTHER -> listOf(
            "Plug the USB-C Ethernet adapter in first.",
            "Open Settings and search for \"Ethernet\" — it usually sits under Network & internet or Connections.",
            "Set IP settings to Static and enter the host IP with a 255.255.255.0 netmask.",
        )
    }

    /**
     * The one caveat worth its own line on screen, per bucket. Kept separate
     * from [steps] so the UI can style it as a warning rather than an
     * instruction.
     */
    fun caveat(oem: Oem = detectOem()): String = when (oem) {
        Oem.PIXEL_AOSP ->
            "Android has no public API for Ethernet IP configuration (EthernetManager is a signature-level @SystemApi), so this has to be done in Settings — no app can do it for you."
        Oem.SAMSUNG ->
            "On some One UI builds the Ethernet entry only exists while an adapter is attached, and static settings reset when the adapter is unplugged. Re-check the address on this screen after re-plugging."
        Oem.XIAOMI ->
            "HyperOS/MIUI has relocated the Ethernet entry between releases and some builds omit static configuration entirely. The address this screen reads back from the interface is the ground truth, not what Settings shows."
        Oem.ONEPLUS_OPPO ->
            "ColorOS/OxygenOS sometimes re-applies DHCP after a reboot or a re-plug. Confirm the address on this screen before every capture."
        Oem.HONOR_HUAWEI ->
            "Several MagicOS/EMUI builds ship no Ethernet settings UI at all. If Settings has no Ethernet entry, static IP is not configurable on this phone and a DHCP-capable switch is the workaround."
        Oem.OTHER ->
            "Ethernet settings placement varies by vendor. Whatever Settings says, the addresses shown on this screen are read back from the live interface and are the ones that matter."
    }

    /**
     * Launches the closest thing to an Ethernet settings screen this device
     * has, most specific first. Returns the label of what was opened, or null
     * if nothing could be resolved (which is itself worth telling the user —
     * it means Settings genuinely has no such screen).
     */
    fun openSettings(context: Context): String? {
        for ((action, label) in candidates()) {
            val intent = Intent(action).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            if (intent.resolveActivity(context.packageManager) != null) {
                context.startActivity(intent)
                return label
            }
        }
        return null
    }

    /**
     * ROUND 25 item 118 — does any of [candidates] actually resolve on this
     * build?
     *
     * The diagnostic wizard shows its "Open Ethernet settings" button **only**
     * when this is true. A button that does nothing on the one build where
     * Settings genuinely has no such screen (several MagicOS/EMUI builds ship
     * none — see [caveat]) is worse than no button: the operator taps it,
     * nothing happens, and they conclude the app is broken rather than that
     * the phone cannot do this.
     *
     * Same `resolveActivity` call [openSettings] makes, so the two cannot
     * disagree.
     */
    fun canOpenSettings(context: Context): Boolean = candidates().any { (action, _) ->
        Intent(action).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            .resolveActivity(context.packageManager) != null
    }

    /**
     * Ordered most-specific to least. None of these is guaranteed to exist:
     * `resolveActivity` is what keeps an unresolvable one from throwing, and
     * is why this is a list rather than one call.
     */
    private fun candidates(): List<Pair<String, String>> = buildList {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // AOSP's Ethernet screen has no public action constant; the
            // wireless-settings screen is its documented parent and is
            // where every vendor's entry hangs off.
            add(Settings.ACTION_WIRELESS_SETTINGS to "Network settings")
        }
        add(Settings.ACTION_WIRELESS_SETTINGS to "Network settings")
        add(Settings.ACTION_SETTINGS to "Settings")
    }
}
