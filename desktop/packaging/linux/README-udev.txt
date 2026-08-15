LidarScan — COIN-D6 USB serial access on Linux
==============================================

If the capture window shows no serial ports, or opening one fails with
"Permission denied", the udev rule is not installed.

The .deb package installs it automatically. An AppImage cannot write to /etc,
so if you are running the AppImage, install it by hand once:

    sudo install -m644 99-lidarscan.rules /etc/udev/rules.d/99-lidarscan.rules
    sudo udevadm control --reload-rules
    sudo udevadm trigger

Then unplug and re-plug the D6.

(This file and 99-lidarscan.rules live in usr/share/lidarscan/ inside the
AppImage. Extract them with:  ./LidarScan-*.AppImage --appimage-extract )

The alternative, if you would rather not add a system rule, is to add yourself
to the `dialout` group and log out and back in:

    sudo usermod -aG dialout "$USER"

Both work. The udev rule is preferred because it grants access to whoever is
actually logged in at the machine, rather than permanently to a group.

Wayland
-------
LidarScan's 3D viewport does not support Wayland yet — it needs an X11 surface.
The bundled .desktop file starts the app with QT_QPA_PLATFORM=xcb so it goes
through XWayland automatically. If you launch the binary directly from a
terminal on a Wayland session, set that yourself:

    QT_QPA_PLATFORM=xcb ./LidarScan-*.AppImage

The Livox Mid-360 is Ethernet, not USB. It needs no driver and no udev rule —
it needs a static IP on the network interface it is plugged into.
