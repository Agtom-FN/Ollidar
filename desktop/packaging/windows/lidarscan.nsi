;; ---------------------------------------------------------------------------
;; lidarscan.nsi — NSIS 3 installer for LidarScan Desktop (Windows 10/11 x64).
;;
;; Tech Spec §3.13 / C8: "NSIS/MSIX installer incl. CH340 driver pointer".
;; NSIS was chosen over MSIX because MSIX requires a code-signing certificate
;; to install at all (even for testing), forces the packaged-app identity model
;; onto a desktop app that talks to raw COM ports, and would still need a
;; separate un-packaged build for CI smoke tests. NSIS produces a plain signed
;; .exe that works on a bare Windows 10 box.
;;
;; !! NOT BUILT OR RUN ON WINDOWS !!
;;   This repo's only host is macOS (NOTES.md §6). The script is syntax-checked
;;   on macOS with `makensis -V2 -DSTUB_CHECK ...` (see build_installer.ps1's
;;   header and packaging/README.md), which parses every line and resolves every
;;   macro, but it has never produced an installer that anyone has run. Treat
;;   the first real Windows CI run as the actual verification.
;;
;; INPUT (produced by build_installer.ps1 / the CI job):
;;   staging\                 windeployqt output + lidarscan.exe + points.filamat
;; OUTPUT:
;;   LidarScan-<version>-x64-setup.exe
;;
;; Build:
;;   makensis /DVERSION=0.2.1 /DSTAGE_DIR=staging lidarscan.nsi   (or omit /DVERSION
;;   to take it from the repo-root VERSION file)
;; ---------------------------------------------------------------------------

Unicode true

; Owner rule (2026-08-17): the version comes from the repo-root VERSION file.
; /DVERSION on the makensis command line still wins (CI passes CMake's
; LIDARSCAN_VERSION, which is read from that same file); this is the fallback for
; a hand-run build, and it reads the file rather than freezing a literal that
; would quietly ship the wrong number.
!ifndef VERSION
  !searchparse /file "..\..\..\VERSION" "" VERSION ""
!endif
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef STAGE_DIR
  !define STAGE_DIR "staging"
!endif

!define APPNAME       "LidarScan"
!define COMPANY       "LidarScan"
!define EXENAME       "lidarscan.exe"
!define REGKEY        "Software\${COMPANY}\${APPNAME}"
!define UNINSTKEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
!define CH340_URL     "https://www.wch-ic.com/downloads/CH341SER_EXE.html"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

Name "${APPNAME} ${VERSION}"
OutFile "LidarScan-${VERSION}-x64-setup.exe"
; 64-bit only: the engine ships x64 (Tech Spec §1 "Windows 10/11 x64") and
; Filament's Windows release has no 32-bit slice.
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "${REGKEY}" "InstallDir"
RequestExecutionLevel admin          ; HKLM + Program Files + the file assoc
SetCompressor /SOLID lzma
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} Desktop installer"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "LegalCopyright"  "${COMPANY}"

;; --- UI --------------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_ICON   "lidarscan.ico"
!define MUI_UNICON "lidarscan.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
; The driver-pointer page is shown at the END of a successful install rather
; than as an extra wizard page: the user needs it only when they plug a D6 in,
; and CaptureWindow's own per-OS hint text already names CH340. Making it a
; finish-page checkbox keeps it discoverable without gating the install.
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APPNAME}"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\drivers\CH340-driver.html"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show COIN-D6 (CH340) USB driver instructions"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION ShowDriverPage
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function ShowDriverPage
  ExecShell "open" "$INSTDIR\drivers\CH340-driver.html"
FunctionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APPNAME} requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

;; --- sections --------------------------------------------------------------
Section "${APPNAME} (required)" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; The whole windeployqt staging tree: lidarscan.exe, Qt6Core/Gui/Widgets/
  ; Network/SerialPort DLLs, platforms\qwindows.dll, styles\, imageformats\,
  ; the MSVC runtime redistributable DLLs windeployqt copies, and
  ; points.filamat (which ViewportWindow loads from applicationDirPath()).
  File /r "${STAGE_DIR}\*.*"

  ; Driver pointer page + icon.
  SetOutPath "$INSTDIR\drivers"
  File "CH340-driver.html"
  SetOutPath "$INSTDIR"
  File "lidarscan.ico"

  WriteRegStr HKLM "${REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "Version"    "${VERSION}"

  ; Add/Remove Programs.
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayName"     "${APPNAME} ${VERSION}"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayIcon"     "$INSTDIR\lidarscan.ico"
  WriteRegStr   HKLM "${UNINSTKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKLM "${UNINSTKEY}" "Publisher"       "${COMPANY}"
  WriteRegStr   HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" "$0"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Start menu shortcut" SecStartMenu
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut  "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\lidarscan.ico"
  CreateShortCut  "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\lidarscan.ico"
SectionEnd

Section "File associations (.lscan.zip, .lscan)" SecAssoc
  ; The Windows half of C7's file association. macOS does this through
  ; Info.plist's CFBundleDocumentTypes; Windows does it through these registry
  ; keys, and both end up calling the same code — Qt turns the command-line
  ; path into the app's own import path (MainWindow's transfer-bundle import;
  ; QEvent::FileOpen is macOS-only, on Windows the path arrives in argv).
  ;
  ; NOTE the double-extension problem: Windows has no concept of a ".lscan.zip"
  ; extension — it only ever sees ".zip", which is owned by the shell's own zip
  ; handler. Claiming ".zip" outright would hijack every zip file on the
  ; machine, which is unacceptable. So:
  ;   * ".lscan" (the project directory / a single-file project archive) IS
  ;     claimed outright, and
  ;   * ".lscan.zip" is exposed as an "Open with LidarScan" verb on .zip files
  ;     rather than as the default handler.
  WriteRegStr HKLM "Software\Classes\.lscan" "" "LidarScan.Project"
  WriteRegStr HKLM "Software\Classes\LidarScan.Project" "" "LidarScan Project"
  WriteRegStr HKLM "Software\Classes\LidarScan.Project\DefaultIcon" "" "$INSTDIR\lidarscan.ico,0"
  WriteRegStr HKLM "Software\Classes\LidarScan.Project\shell\open\command" "" '"$INSTDIR\${EXENAME}" "%1"'

  WriteRegStr HKLM "Software\Classes\LidarScan.TransferBundle" "" "LidarScan Transfer Bundle"
  WriteRegStr HKLM "Software\Classes\LidarScan.TransferBundle\DefaultIcon" "" "$INSTDIR\lidarscan.ico,0"
  WriteRegStr HKLM "Software\Classes\LidarScan.TransferBundle\shell\open\command" "" '"$INSTDIR\${EXENAME}" "%1"'

  ; Register the app in OpenWithProgids for .zip so "Open with > LidarScan"
  ; appears on transfer bundles without stealing the .zip default.
  WriteRegStr HKLM "Software\Classes\.zip\OpenWithProgids" "LidarScan.TransferBundle" ""

  ; Applications\lidarscan.exe advertises supported types to the Open With UI.
  WriteRegStr HKLM "Software\Classes\Applications\${EXENAME}\shell\open\command" "" '"$INSTDIR\${EXENAME}" "%1"'
  WriteRegStr HKLM "Software\Classes\Applications\${EXENAME}\SupportedTypes" ".lscan" ""
  WriteRegStr HKLM "Software\Classes\Applications\${EXENAME}\SupportedTypes" ".zip" ""

  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
SectionEnd

Section "COIN-D6 USB driver info (CH340)" SecDriver
  ; DELIBERATELY NOT BUNDLING THE DRIVER BINARY.
  ; CH341SER.EXE is WCH's redistributable and its licence terms for
  ; third-party bundling are not something this task can clear, the file is
  ; version-churny, and a stale bundled driver is worse than none. Windows 10
  ; and 11 also ship an in-box CH340 driver that works for most units, so the
  ; common case needs no download at all. What the installer therefore ships is
  ; a local page that (a) says "try plugging it in first", (b) tells the user
  ; how to check Device Manager, and (c) links WCH's official download.
  ; This matches what CaptureWindow's per-OS guidance text already tells users.
  SetOutPath "$INSTDIR\drivers"
  File "CH340-driver.html"
SectionEnd

LangString DESC_SecApp       ${LANG_ENGLISH} "The ${APPNAME} application and the Qt runtime it needs."
LangString DESC_SecStartMenu ${LANG_ENGLISH} "Add ${APPNAME} to the Start menu."
LangString DESC_SecDesktop   ${LANG_ENGLISH} "Add a shortcut to the Desktop."
LangString DESC_SecAssoc     ${LANG_ENGLISH} "Open .lscan projects and .lscan.zip transfer bundles with ${APPNAME}."
LangString DESC_SecDriver    ${LANG_ENGLISH} "Install a local page with COIN-D6 (CH340) USB-serial driver instructions."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp}       $(DESC_SecApp)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} $(DESC_SecStartMenu)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop}   $(DESC_SecDesktop)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc}     $(DESC_SecAssoc)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver}    $(DESC_SecDriver)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;; --- uninstall -------------------------------------------------------------
Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir  "$SMPROGRAMS\${APPNAME}"

  DeleteRegKey HKLM "Software\Classes\.lscan"
  DeleteRegKey HKLM "Software\Classes\LidarScan.Project"
  DeleteRegKey HKLM "Software\Classes\LidarScan.TransferBundle"
  DeleteRegValue HKLM "Software\Classes\.zip\OpenWithProgids" "LidarScan.TransferBundle"
  DeleteRegKey HKLM "Software\Classes\Applications\${EXENAME}"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "${REGKEY}"

  ; User data (projects, QSettings) is NOT removed: .lscan projects are the
  ; user's captured field data and an uninstaller must never delete them.
  RMDir /r "$INSTDIR"

  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
SectionEnd
