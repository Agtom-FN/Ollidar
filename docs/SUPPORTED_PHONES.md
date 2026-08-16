# LidarScan Android — Supported Phones

**Reference device: Google Pixel 8 Pro.** Every Android field-test pass in
this repo (`tools/fieldtest-kit/android/TEST_GUIDE.md`) is written against
it, and it's the only phone the app has been built and (partially) run
against so far. Everything below this point is derived requirements plus
public compatibility data, not per-device field verification — see
"Tested-list philosophy" at the end before treating any row as a promise.

---

## 1. Hard requirements (the app will not install, or will not do anything
useful, without these)

| Requirement | Why | Source |
| --- | --- | --- |
| **`arm64-v8a` CPU ABI** | `libscanengine_jni.so` (the engine, JNI-wrapped) is compiled for `arm64-v8a`; that's the only ABI a real phone needs (the debug APK also ships an `x86_64` slice, but that exists solely so an x86_64 emulator can run CI — no real phone uses it). An `armeabi-v7a`-only (32-bit) phone cannot install a build that only ships 64-bit native code the way this one is configured. | `android/app/build.gradle.kts` `abiFilters`; `android/NOTES.md` B2 §1 |
| **Android 10 (API 29) or newer** | `minSdk = 29` in the app's own build config. | `android/app/build.gradle.kts` |
| **USB host / USB OTG support** | The D6 (USB-serial) and the Mid-360 (USB-C-to-Ethernet adapter) both require the phone to act as a USB *host*, not just a USB peripheral/charging port. Almost all current-generation Android phones support this over USB-C, but budget models and some tablets omit it. | `android/AndroidManifest.xml` declares `android.hardware.usb.host` as required; `engine/docs/A2-d6-driver.md`, `engine/docs/A3-mid360-driver.md` |
| **Vulkan 1.1** (practically: any GPU capable of OpenGL ES 3.1+/Vulkan 1.1, which is essentially every phone sold since ~2018) | Filament (the point-cloud renderer) targets both OpenGL ES and Vulkan backends at the `mobile` feature tier; the material is compiled for both (`matc -a opengl -a vulkan -p mobile`). This is a very low bar in practice — it is listed because it is a real, checkable requirement, not because it is expected to exclude anything a buyer would consider today. | `android/app/build.gradle.kts` `compileMaterials`; `android/NOTES.md` B4 §7 |

## 2. Soft requirement — ARCore ("Google Play Services for AR")

ARCore is **not** a hard requirement to install or open the app, but it
gates specific features, and the honest picture (verified by reading the
app's own availability-check code, not assumed) is:

**Without ARCore (device unsupported, or Play Services for AR missing/out of
date):**
- The AR camera overlay during capture is unavailable.
- The mount-calibration wizard (the ARCore-driven "hold the rig steady while
  you tilt through 8 poses" flow) is unavailable — a rig cannot get a
  mount-calibration record without it.
- The D6's live pushbroom trajectory (D6 has no self-localization of its
  own — it needs a pose source to turn raw scans into a registered map, and
  ARCore is that pose source on Android) is unavailable, so D6 live-SLAM
  capture degrades.
- Colorization is unavailable for any capture made without ARCore, because
  it needs the camera keyframes ARCore tracking produces.

**What still works without ARCore:** raw sensor **capture and recording**
for both D6 and Mid-360 — the underlying driver, USB/Ethernet transport, and
`.lscan` recording do not depend on ARCore at all. The Mid-360 in particular
has its own onboard LIO (lidar-inertial odometry) for live-SLAM, so a
Mid-360 rig's live map is not affected by ARCore's absence the way D6's is.
Post-processing, export, plan-view extraction, and RTK/georeferencing are
all independent of ARCore too. In short: **a phone with no ARCore support
can still be a working capture device for record-only or Mid-360 sessions;
it loses the AR-assisted parts of the workflow**, exactly as the app's own
in-code message states it (`ArAvailability.UNSUPPORTED`: *"This device does
not support ARCore, so the AR overlay and the D6 pushbroom trajectory are
unavailable"*).

This has been read out of the app's source, not run on an unsupported
device — no such device was available while building this list.

## 3. Notable non-requirement, with a real caveat — USB-C Ethernet

There is no Android API to certify "this phone's USB-C port + this
particular Ethernet dongle will work," and it varies by OEM in two
independent ways documented in the app's own connect-wizard code
(`android/app/src/main/kotlin/com/lidarscan/app/net/StaticIpGuidance.kt`):

1. **Whether the adapter is recognized at all** depends on the adapter's
   chipset, not the phone — ASIX AX88179 and Realtek RTL8153 both have
   in-box Android kernel driver support; other chipsets are not guaranteed.
2. **Whether the phone's Settings app exposes a static-IP form for Ethernet**
   is OEM- and build-dependent even when the adapter itself is recognized —
   see the per-OEM notes in the table below. This is why "tested" for a
   phone in this doc's tables means specifically "the Ethernet
   adapter-recognition + static-IP flow has been walked on this phone," not
   just "it's an Android phone."

---

## 4. Curated device table

Sourced from Google's official ARCore-supported-devices list
(`developers.google.com/ar/devices`, queried 2026-08-16) plus general Android
version/USB-host knowledge. "Tested" below means *walked in this repo's own
field-test process* — as of this writing, that's the Pixel 8 Pro only;
everything else is "should work" derived from the requirements in §1–3, not
a field pass.

### Tier 1 — Reference device

| Device | Status | Notes |
| --- | --- | --- |
| **Google Pixel 8 Pro** | **Reference — tested** | The device this app, `TEST_GUIDE.md`, and every field-test checklist item are written against. ARCore-supported with Depth API. Stock Android Ethernet path documented in `TEST_GUIDE.md` §2(b). |

### Tier 1 — Recommended (ARCore-certified, current-generation, arm64,
Android 14+, should need no workarounds beyond the documented Ethernet
static-IP steps)

| Device | ARCore | Notes / USB-Ethernet quirks |
| --- | --- | --- |
| Pixel 7 / 7 Pro / 7a | Supported, Depth API | Same AOSP-stock Ethernet Settings path as the 8 Pro is expected (not yet walked); no known adapter issues reported. |
| Pixel 9 / 9 Pro / 9 Pro XL / 9 Pro Fold / 9a | Supported, Depth API | Same as above. |
| Pixel 10 / 10 Pro / 10 Pro Fold | Supported, Depth API | Newest Pixel line as of this writing; same stock path expected. |
| Samsung Galaxy S22 / S22+ / S22 Ultra | Supported, Depth API | One UI's Ethernet path is a **different menu** than Pixel — `Settings → Connections → More connection settings → Ethernet` per the app's own in-wizard guidance. Some One UI builds only show the Ethernet entry while an adapter is physically attached, and static settings have been reported to reset on unplug/replug — re-verify the address after reconnecting. |
| Samsung Galaxy S23 / S23+ / S23 Ultra / S23 FE | Supported, Depth API | Same One UI caveat as S22. |
| Samsung Galaxy S24 / S24+ / S24 Ultra / S24 FE | Supported, Depth API | Same One UI caveat as S22. |
| Samsung Galaxy S25 Ultra | Supported, Depth API | Same One UI caveat as S22; other S25 variants not yet listed by Google as of this query — recheck before relying on them. |

### Tier 2 — Should work, untested (ARCore-supported per Google's list,
meets the arm64/Android 10+/USB-host bar, but not a phone this app has any
first-hand data on — different OEM skins carry real, documented Ethernet-UI
variance)

| Family | Examples | Known USB-Ethernet quirk (if any) |
| --- | --- | --- |
| **OnePlus** | OnePlus 11/12/13 series, OnePlus Open, Nord 4/5 | ColorOS/OxygenOS: `Settings → Wi-Fi & network (or Connection & sharing) → Ethernet`. Some builds re-apply DHCP after a reboot or re-plug — confirm the address every session. |
| **Xiaomi / Redmi / POCO** | Xiaomi 13/14T/15 series, Redmi Note 13/14 Pro, POCO F5/X7 Pro | HyperOS/MIUI: `Settings → Connection & sharing → Ethernet`, but the entry has moved across releases — search Settings for "Ethernet" if it's not where expected. Static config is reported absent on some builds entirely. |
| **Motorola** | Recent `edge` and `moto g` 5G lines | Ethernet path and static-IP support not documented per-model; treat as "search Settings for Ethernet" until verified. |
| **Nothing** | Phone (2), (2a), (3), (3a) | No specific Ethernet-UI reports found; ARCore-supported. Treat as untested for the USB-Ethernet flow specifically. |
| **HONOR / Huawei** (non-HMS models still shipping Google Play) | Select HONOR models with Google Play | **Worst-case OEM for this feature**: several MagicOS/EMUI builds ship **no Ethernet settings UI at all**. If Settings has no Ethernet entry, static IP is not configurable on that phone — the documented workaround is a DHCP-capable switch between the phone and the Mid-360 rather than a direct cable. |

### Explicit "unsupported" — no ARCore

Any Android phone **not** on Google's official ARCore device list
(`developers.google.com/ar/devices`) — this includes most budget/entry-level
phones, many phones sold without Google Play (some regional variants), and
anything below Android's practical ARCore floor. Per §2 above: **the app
still installs and captures raw sensor data on such a device** (assuming it
otherwise meets §1's hard requirements — arm64, Android 10+, USB host), but
loses the AR overlay, the mount-calibration wizard, D6's live pushbroom
trajectory, and colorization. This has been verified by reading the app's
own degradation logic, not by running the app on a specific unsupported
device.

---

## 5. Tested-list philosophy

This table will always be shorter than "phones that would probably work" —
that's deliberate. A phone earns a row above the "should work, untested"
tier only after someone has actually run `TEST_GUIDE.md`'s sideload +
per-sensor pass on it and reported back. Two things follow from that:

- **"Should work, untested" is not a guess about capability** — every device
  on Google's ARCore list plus a real arm64-v8a/Android-10+/USB-host chip
  genuinely should run the app. It's a guess about **friction**: exactly
  which submenu the Ethernet static-IP form lives in, whether Save is
  greyed out until specific fields are filled, whether a given USB-C
  dongle's chipset is what its box claims. Those are the details that eat a
  field session, and they are OEM-build-specific in ways no spec sheet
  states.
- **Every field-test report that comes back should update this table** —
  move a device up a tier, add a discovered Ethernet-menu path, or flag a
  phone that turned out not to work despite meeting the ARCore/arm64 bar.
  This file is meant to be a living record of what has actually been
  walked, the same spirit as `docs/bench/TEST_CHECKLIST.md`'s "re-run
  repeatedly, not filled in once."

**Sources**

- [ARCore supported devices](https://developers.google.com/ar/devices) —
  Google's official list, queried 2026-08-16 for Pixel 7/8/9/10 series,
  Samsung Galaxy S22–S25 series, OnePlus/Xiaomi/Motorola/Nothing device
  presence and Depth API support.
- `android/app/build.gradle.kts`, `android/AndroidManifest.xml`,
  `android/app/src/main/kotlin/com/lidarscan/app/ar/ArAvailability.kt`,
  `android/app/src/main/kotlin/com/lidarscan/app/net/StaticIpGuidance.kt` —
  this repo's own source, for the hard requirements and the per-OEM Ethernet
  guidance already written into the app.
- `android/NOTES.md` (B2, B3, B4, B7/B8, B9 sections) — engineering record
  of what's been built, what's been verified on-device (an emulator, not a
  physical phone), and what's still hardware-deferred.
