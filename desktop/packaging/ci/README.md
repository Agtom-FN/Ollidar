# Staged CI jobs for C8 packaging

**These are `.yml.snippet` files, not workflows. Nothing in this directory runs.**

`.github/` is owned by another agent this wave, so C8 cannot add or edit a
workflow file. The jobs below are written, complete and ready; whoever owns
`.github/workflows/` merges them into the release workflow. That is a
copy-paste of the `jobs:` entries plus whatever `on:` / `permissions:` the
target workflow already declares — the snippets deliberately contain **only**
job definitions so they drop in without conflicting.

| File | Job | Runner | Produces |
| --- | --- | --- | --- |
| `macos-dmg.yml.snippet` | `macos-universal-dmg` | `macos-14` (arm64) | `LidarScan-<v>-universal.dmg` |
| `windows-installer.yml.snippet` | `windows-installer` | `windows-2022` | `LidarScan-<v>-x64-setup.exe` |
| `linux-packages.yml.snippet` | `linux-packages` | `ubuntu-22.04` | `.AppImage` + `.deb` |

## What is real and what is not

* **macOS** — the job runs exactly the four scripts that produced a real,
  mounted, launched DMG on the development machine (see `desktop/NOTES.md`
  §13). The only step in it that has never run anywhere is notarization, which
  is gated behind `secrets.APPLE_*` being present and skipped otherwise.
* **Windows / Linux** — **these packaging jobs have still never been executed on
  any machine.** `lidarscan.nsi` has been compiled by `makensis` on macOS against
  stub files (a real 146 KB installer came out), and `lidarscan.desktop` passes
  `desktop-file-validate`, but that validates *syntax*, not that the installers
  build. Expect the first run of each to fail, and treat fixing it as real work.

  **What HAS changed since these snippets were written:** the *app itself* now
  builds on both platforms. `.github/workflows/desktop-ci.yml` (a real workflow,
  not a snippet — see `NOTES.md` §14) compiles every `desktop/src` translation
  unit on `windows-latest` and `ubuntu-latest` and links a real `lidarscan.exe`
  on Windows. So the premise these two snippets were written under — "the app
  does not build on this platform, so the installer is groundwork" — no longer
  holds, and whoever picks them up should start from `desktop-ci.yml`'s Qt and
  Filament steps, which are known-good, rather than the ones drafted here.
  In particular the Windows snippet's `tar xzf ... -C third_party/filament` is
  **wrong** — the Windows tarball has no top-level `filament/` directory; use
  `tools/fetch_filament.ps1`. `NOTES.md` §3.1 still marks both **renderers**
  UNVERIFIED, because nothing has been run.

## Caching

Each job caches the expensive, deterministic input:

* macOS: `third_party/filament-x86_64` keyed on the Filament tag — this is the
  ~2 minute from-source x86_64 build, and it only changes when the pinned
  Filament version does. `third_party/qt-universal` is keyed on the Qt version.
* Windows/Linux: the Qt install and the Filament release tarball.

Without the caches the macOS job is roughly 6 minutes longer per run.

## Secrets the macOS job wants (all optional)

| Secret | Used for |
| --- | --- |
| `APPLE_CERT_P12` / `APPLE_CERT_PASSWORD` | the Developer ID Application certificate, imported into a throwaway keychain |
| `APPLE_SIGNING_IDENTITY` | e.g. `Developer ID Application: ACME Ltd (AB12CD34EF)` |
| `APPLE_ID` / `APPLE_APP_PASSWORD` / `APPLE_TEAM_ID` | `notarytool submit` credentials |

With none of them set the job still produces an ad-hoc-signed DMG and says so
in its summary — which is exactly what the development machine produced. See
`desktop/packaging/PACKAGING.md` for the full signing and notarization
procedure the secrets feed.
