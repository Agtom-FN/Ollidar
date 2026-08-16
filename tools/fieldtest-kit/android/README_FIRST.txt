LidarScan Android field-test kit — quick start

1. Sideload LidarScan-debug.apk onto your Pixel 8 Pro: open it from the
   Files app (Downloads) and tap through the install prompt, or run
   `adb install -r -t LidarScan-debug.apk` from a laptop.
2. Open TEST_GUIDE.md in this folder and follow it in order — sideload
   check, then D6, then Mid-360, then the UM982 reality check (read this
   one BEFORE unboxing the UM982), then the capture/export/plan pass list.
3. This build has barely run on real hardware yet, so treat surprises as
   data, not just bugs — note exact numbers/screens/errors, not just
   "didn't work."
4. If the app crashes: `adb logcat -d -b crash > crash.txt`, or note which
   screen/button and what you saw.
5. Report back using the checklist at the end of TEST_GUIDE.md, including
   the phone-list notes if you test on anything besides the Pixel 8 Pro —
   see docs/SUPPORTED_PHONES.md in the main repo.
