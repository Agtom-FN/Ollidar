# LidarScan desktop packaging (C8)

How the shippable artifacts for each OS are produced, what has actually been
run, and what a real signed/notarized release needs on top.

Spec references: `docs/LidarScan Tech Spec.md` §3.13 and the Workstream C table
row C8 — *"notarized universal DMG (Intel + Apple Silicon) · Windows NSIS/MSIX
installer + CH340 driver guidance · Linux AppImage + .deb with udev rule"*.

---

## 0. Status at a glance

| Target | Script | Ever run? | Result |
| --- | --- | --- | --- |
| macOS universal DMG | `tools/package_macos.sh` | **Yes, in full** | `dist/LidarScan-0.1.0-universal.dmg`, 30 MB. Mounted, launched, both slices exercised. **Ad-hoc signed, NOT notarized.** |
| Windows x64 installer | `packaging/windows/build_installer.ps1` | No | `lidarscan.nsi` compiles under `makensis` on macOS against stubs (real 146 KB `.exe` out). Nothing else validated. |
| Linux AppImage + .deb | `packaging/linux/build_{appimage,deb}.sh` | No | `.desktop` passes `desktop-file-validate`, MIME XML is well-formed. Nothing else validated. |

The macOS half is real. The Windows and Linux halves are staged, and the honest
reason is in `NOTES.md` §3.1: **neither renderer has ever been compiled**, let
alone run. Packaging scripts for a platform whose app does not build are useful
groundwork, not a deliverable anyone should trust.

---

## 1. macOS

### 1.1 The two blockers that made this task non-trivial

Both are "the thing you would reach for is single-architecture, and you do not
find out until the link step".

**Filament ships arm64 only.** `filament-v1.75.0-mac.tgz` contains `lib/arm64`
and nothing else — no `lib/x86_64`, no fat archives. C1 found this and recorded
it in `NOTES.md` §3.2 as a C8 blocker. Options were: ship two DMGs, drop Intel,
or build the missing slice. **Owner decision: build it.**
`tools/build_filament_x86_64.sh` does, `tools/make_universal_filament.sh` lipos
the result against the prebuilt arm64 set. See §1.2.

**Homebrew Qt is arm64 only.** `lipo -info /opt/homebrew/opt/qt/lib/QtCore.framework/QtCore`
→ `Non-fat file: ... is architecture: arm64`. Homebrew builds per-architecture
bottles by design and there will never be a universal one, so a universal
LidarScan.app cannot be linked against Homebrew Qt at all. The Qt Company's own
macOS desktop binaries **have been universal since Qt 6.2** — the archive names
say it out loud (`qtbase-MacOS-MacOS_15-Clang-MacOS-MacOS_15-X86_64-ARM64.7z`).
`tools/fetch_qt_universal.sh` fetches them with `aqtinstall` (non-interactive,
no Qt account, same CDN as the official installer) into
`third_party/qt-universal/`. Qt stays **dynamically linked**, which Tech Spec §1
requires for the LGPLv3 terms this project relies on.

### 1.2 Building the x86_64 Filament slice

```sh
cd desktop
./tools/fetch_filament.sh v1.75.0          # arm64, prebuilt, 131 MB unpacked
./tools/build_filament_x86_64.sh v1.75.0   # x86_64, from source
./tools/make_universal_filament.sh         # lipo -> third_party/filament-universal/
```

Cross-compiling is just `-DCMAKE_OSX_ARCHITECTURES=x86_64` — Filament's CMake
honours it. The catch is that the build **runs tools it just built**: `matc`
compiles Filament's ~40 built-in materials and `resgen` turns the results into
assembly that is compiled into `libfilament.a`. Under an x86_64 configure those
tools are x86_64 too, i.e. the build wants to execute Intel binaries on an
Apple-Silicon host.

Two ways out:

* **Rosetta 2** — the x86_64 `matc`/`resgen` are translated transparently and
  the build just works. This is what was used. The script preflights it and
  refuses with instructions if it is missing.
* **`-DIMPORT_EXECUTABLES_DIR=<arm64 build dir>`** — Filament's own
  cross-compilation escape hatch (how its Android/iOS/WebGL builds get host
  tools). It needs a *complete prior arm64 CMake build tree of the same
  revision*, because it consumes that build's generated
  `ImportExecutables-Release.cmake`. The prebuilt release tarball is not such a
  tree, so this route means building Filament twice. The script accepts
  `IMPORT_EXECUTABLES_DIR` in the environment if Rosetta is ever unavailable.

Only the 12 libraries the app links are built (`filament backend bluegl bluevk
filabridge filaflat utils geometry smol-v ibl abseil zstd`), passed to ninja as
explicit targets. That skips gltfio, viewer, matdbg, samples, image/imageio,
assimp, the tests and the Java bindings — roughly two thirds of a full build,
and the reason this takes **2 minutes** rather than the 30–60 that was budgeted.

Two gotchas the script carries as comments, because both are silent:

1. **`ninja abseil` does not exist.** The target is `filament-abseil` (Filament
   prefixes its vendored abseil so it cannot collide with a system one); the
   release renames it to `libabseil.a` on the way into the tarball.
2. **`geometry` and `abseil` ship as *combined* archives, not the target
   outputs.** Both use Filament's `combine_static_libs()` helper
   (`CMakeLists.txt:777`), a POST_BUILD step that merges the target and all its
   static dependencies into `lib<name>_combined.a`; `install(FILES … RENAME)`
   is what ships it under the plain name. The plain ninja output for
   `filament-abseil` is an **empty 656-byte archive** — copy that and everything
   configures, links, and then dies on undefined absl symbols. The script
   prefers `*_combined.a` and has a "no archive under 4 KB" tripwire for the
   general case.

`matc` is **not** built for x86_64 and does not need to be: it is a build-time
tool that runs on the developer's/CI's machine, and the arm64 one from the
prebuilt release compiles `materials/points.mat` for either target.

### 1.3 Building and packaging the app

```sh
./tools/fetch_qt_universal.sh 6.11.1
./tools/make_icon.sh
./tools/package_macos.sh
```

`package_macos.sh` configures with `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`,
builds (the engine builds universal from the same flag — its own
`macos-universal` preset already proved that), runs `macdeployqt` from the
**universal** Qt, signs inside-out, and makes the DMG with `create-dmg` if
present or `hdiutil` otherwise.

`CMakeLists.txt` keys off `CMAKE_OSX_ARCHITECTURES`: asking for both
architectures makes the universal Filament tree **required**, with a fatal error
naming the three scripts to run. Falling back to the arm64-only prebuilt would
link with warnings and produce an app whose Intel slice is missing every
Filament symbol — the exact silent failure this task exists to prevent.

The bundle is behind `-DLIDARSCAN_MACOS_BUNDLE=ON`, default **OFF**, because
`scripts/verify*.sh` (all of C1–C7's evidence) runs `build/lidarscan` and moving
the executable into `LidarScan.app/Contents/MacOS/` would break all four.

Two bundle-specific changes:

* `points.filamat` goes into `Contents/Resources`. `ViewportWindow.cpp` now
  searches `applicationDirPath()` first and `../Resources` second, so one binary
  works in both layouts — the one line `NOTES.md` §7 flagged as C8's to change.
* `packaging/Info.plist.in` (written by C7) becomes the real `Info.plist`.
  `CFBundleExecutable` is filled from CMake's `LIDARSCAN_EXECUTABLE`, which is
  also the target's `OUTPUT_NAME`, so the plist and the Mach-O cannot drift
  apart. A mismatch there is a bundle macOS refuses to launch.

### 1.4 What "real signing and notarization" needs

The DMG produced here is **ad-hoc signed** (`codesign -s -`). `spctl -a -t exec`
says **`rejected`**, and that is recorded rather than hidden: on any other Mac
this app is quarantined and refused until notarized (or until the user
right-click → Open, which is not a shipping experience).

To make that line say `accepted`:

**1. A Developer ID Application certificate** — $99/year Apple Developer
Program, then Xcode → Settings → Accounts → Manage Certificates → **Developer ID
Application**, or `developer.apple.com/account/resources/certificates`. Export
it as a `.p12` for CI.

```sh
security find-identity -v -p codesigning     # confirm it is in the keychain
```

**2. Sign with the hardened runtime and entitlements.** Notarization *requires*
`--options runtime`, and the hardened runtime turns off things this app needs.
`packaging/macos/entitlements.plist` requests exactly four, each for a named
reason:

| Entitlement | Needed by |
| --- | --- |
| `com.apple.security.network.client` | Mid-360 UDP, C4's cloud submit (`QtHttpTransport`) |
| `com.apple.security.network.server` | the SDK2 backend binds local ports 56100–56501 to *receive* the sensor's datagrams |
| `com.apple.security.device.usb` + `.serial` | `QSerialPort` on `/dev/cu.usbserial-*` (COIN-D6 / CH340) |
| `com.apple.security.files.user-selected.read-write` | projects, PLY/LAS/PCD exports, DXF/PDF, `.lscan.zip` bundles |

Deliberately **not** requested, with reasons in the file: `allow-unsigned-executable-memory`
(Filament's Metal shader compilation is out-of-process; nothing JITs in this
address space), `disable-library-validation` (every nested dylib is signed with
the same identity by the inside-out pass, so validation already passes), and
`app-sandbox` (this is Developer-ID/DMG distribution, not Mac App Store; the
sandbox is not required for notarization and would break the serial path).

```sh
CODESIGN_IDENTITY="Developer ID Application: ACME Ltd (AB12CD34EF)" \
  ./tools/package_macos.sh
```

**3. Notarize the DMG and staple the ticket.**

```sh
xcrun notarytool store-credentials "lidarscan" \
  --apple-id you@example.com --team-id AB12CD34EF --password <app-specific-password>

xcrun notarytool submit dist/LidarScan-0.1.0-universal.dmg \
  --keychain-profile "lidarscan" --wait

xcrun stapler staple dist/LidarScan-0.1.0-universal.dmg
xcrun stapler validate dist/LidarScan-0.1.0-universal.dmg
spctl -a -vvv -t open --context context:primary-signature dist/LidarScan-0.1.0-universal.dmg
```

The app-specific password comes from appleid.apple.com → Sign-In and Security →
App-Specific Passwords. **Stapling matters**: without it the ticket is only
fetched online, so a first launch on a machine with no network is still blocked.

Common rejections, all of which this build is already set up to avoid:

* *"The signature does not include a secure timestamp"* — needs `--timestamp`
  (the ad-hoc path uses `--timestamp=none`, which is correct for ad-hoc and
  wrong for Developer ID; `package_macos.sh` switches automatically).
* *"The binary is not signed with a valid Developer ID"* on a nested framework —
  inside-out signing, which the script does.
* *"The executable does not have the hardened runtime enabled"* — `--options runtime`.
* Signing the outer bundle with `--deep` and a real identity. `--deep` is fine
  for ad-hoc but Apple explicitly deprecates it for distribution because it
  cannot apply per-binary entitlements. The script only uses it on the ad-hoc
  path.

### 1.5 File association

C7 wrote `Info.plist.in` with `CFBundleDocumentTypes` + `UTExportedTypeDeclarations`
for `.lscan.zip` and `.lscan`, and noted that none of it can work until there is
a real bundle. There now is one, and LaunchServices picked it up from the
mounted DMG (`NOTES.md` §13) — `lsregister -dump` shows the app claiming
`com.lidarscan.transfer-bundle` and `com.lidarscan.project`.

---

## 2. Windows

`packaging/windows/` — NSIS 3 (`lidarscan.nsi`) driven by `build_installer.ps1`,
which builds, runs `windeployqt` into a staging tree, then `makensis`.

**MSIX was considered and rejected**: it cannot be installed at all without a
code-signing certificate, even for a local test; it forces the packaged-app
identity model onto an app that opens raw COM ports; and it would still need a
separate unpackaged build for CI smoke tests. NSIS produces a plain signable
`.exe` that works on a bare Windows 10 box.

**CH340 driver: a pointer page, not a bundled binary.** `CH340-driver.html` is
installed to `<InstallDir>\drivers\` and offered from the installer's finish
page. It tells the user to plug the device in *first* (Windows 10/11 ship an
in-box CH340 driver that works for most units), how to check Device Manager, and
links WCH's official `CH341SER.EXE` download. The driver binary is deliberately
not redistributed: WCH's bundling terms are not something this task can clear,
the file churns, and a stale bundled driver can downgrade a working in-box one.
This matches what `CaptureWindow`'s own per-OS guidance already tells users.

**File association has a real Windows-specific wrinkle.** Windows has no concept
of a `.lscan.zip` extension — it only ever sees `.zip`, owned by the shell's zip
handler. Claiming `.zip` outright would hijack every zip on the machine. So
`.lscan` is claimed as a ProgID outright, and `.lscan.zip` is exposed through
`OpenWithProgids` on `.zip` (an "Open with → LidarScan" entry) rather than as
the default handler.

**Signing.** Authenticode, both the app and the installer — signing only the
installer leaves an unsigned `lidarscan.exe` on disk afterwards:

```
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
    /f cert.pfx /p <password> staging\lidarscan.exe LidarScan-0.1.0-x64-setup.exe
```

An OV certificate still triggers SmartScreen warnings until the binary builds
download reputation; an **EV** certificate gets reputation immediately. That is
a purchasing decision, not a technical one.

**Validation done:** `makensis -V3 -DVERSION=0.1.0 -DSTAGE_DIR=staging
lidarscan.nsi` on macOS (Homebrew `makensis` 3.12) against stub staging files
compiles clean and emits a real 146 KB installer — 6 pages, 5 sections, 760
instructions, LZMA. Log: `desktop/evidence/nsis-syntax-check.log`. That proves
every macro resolves and every command parses. It proves nothing about
behaviour.

---

## 3. Linux

`packaging/linux/` — both artifacts Tech Spec §3.13 asks for, because they do
different jobs:

| | AppImage | .deb |
| --- | --- | --- |
| Qt | bundled (`linuxdeploy --plugin qt`) | depends on the distro's Qt 6 |
| Size | ~50 MB | a few MB |
| Qt security updates | frozen at build time | arrive via apt |
| udev rule | **cannot install it** (carried in `usr/share/lidarscan/` + a README) | installed to `/lib/udev/rules.d/`, `udevadm` reloaded in postinst |
| File association | **cannot register it** | `update-mime-database` in postinst |
| Needs root | no | yes |

**`linuxdeploy`, not `linuxdeployqt`**: the latter hard-refuses to run on
anything newer than the oldest supported Ubuntu LTS, which makes it unusable on
a modern CI image. Both produce an AppImage whose glibc floor is the *build*
machine's, which is why the CI job pins `ubuntu-22.04`.

**The udev rule** (`99-lidarscan.rules`) matches the CH340 (`1a86:7523`, plus
the `7522`/`5523` variants) and FTDI `0403:6001`, and uses `TAG+="uaccess"` so
the device belongs to whoever is logged in at the seat — rather than requiring
`usermod -aG dialout` and a re-login, which is a poor first-run experience for a
field tool. There is a `/dev/lidarscan-d6` convenience symlink, documented as
*not* something the app depends on (`QSerialPortInfo` enumerates real device
nodes, not symlinks).

**Wayland is the live risk.** `NativeSurface_linux.cpp` refuses a Wayland
surface with an explanatory error, so `lidarscan.desktop` launches with
`QT_QPA_PLATFORM=xcb` to force XWayland. On Ubuntu 22.04+ and Fedora, Wayland is
the default session, so without that line the 3D viewport does not come up at
all. Remove it the day the Wayland path is real.

**Validation done:** `desktop-file-validate lidarscan.desktop` → clean;
`xmllint --noout lidarscan.xml` → well-formed. `shellcheck -S warning` is clean
across all nine shell scripts this task added.

---

## 4. CI

`packaging/ci/*.yml.snippet` — three staged jobs (macOS DMG, Windows installer,
Linux packages). They are snippets, not workflows, because `.github/` is owned
by another agent this wave. `packaging/ci/README.md` says what each one does,
what it caches, and which secrets it wants. All three parse as valid YAML.

---

## 5. Reproducing the macOS DMG from a clean checkout

```sh
cd desktop
./tools/fetch_filament.sh v1.75.0          # ~40 s   (44 MB download)
./tools/build_filament_x86_64.sh v1.75.0   # ~7 min  (clone 3 min + configure + 2 min build)
./tools/make_universal_filament.sh         # ~2 s
./tools/fetch_qt_universal.sh 6.11.1       # ~1 min  (1.3 GB)
./tools/make_icon.sh                       # ~2 s
./tools/package_macos.sh                   # ~3 min
```

→ `dist/LidarScan-0.1.0-universal.dmg`.

Everything under `third_party/`, `build-universal/` and `dist/` is gitignored;
only the scripts are committed.
