package com.lidarscan.app.net

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager

/**
 * ROUND 25 item 118 — **what the OS DOES enumerate on USB.**
 *
 * The owner's field failure produced `no-ethernet — No Ethernet adapter`,
 * twice, and stopped there. That line cannot distinguish "nothing is plugged
 * in" from "something is plugged in that the kernel will not drive" from "the
 * adapter is browning out on bus power", and those three have three different
 * fixes (a cable, a different adapter, a *powered* hub).
 *
 * The evidence that separates them costs one `UsbManager.getDeviceList()`
 * call: **an adapter the kernel has no driver for still enumerates on USB.**
 * So an empty list is genuinely "nothing plugged in", and a non-empty list
 * with no Ethernet network is hardware that is present and not working —
 * which is the answer the operator needs, and the answer a field report a
 * month later needs even more.
 *
 * ## Why the names are formatted the way they are
 *
 * `productName` is what a human recognises ("AX88179A Gigabit Ethernet") and
 * is null on plenty of devices; the VID:PID always exists and is the thing
 * that can be looked up against a kernel driver table. Both go on the line,
 * because the screen is read by an operator and the log is read by whoever is
 * working out whether `r8152` is in this phone's kernel.
 *
 * Deliberately **read-only and permission-free**: enumerating the device list
 * needs no `UsbManager` permission grant (only *opening* a device does), so
 * this can run on a poll in a wizard without ever showing a dialog.
 */
class UsbDeviceNames(context: Context) {

    private val appContext = context.applicationContext

    /**
     * Every USB device the OS currently enumerates, newest formatting first.
     * Empty when the platform has no `UsbManager` at all — which reads the
     * same as "nothing plugged in" and is the safe way round: the diagnostic
     * then shows the generic guidance rather than claiming hardware is
     * present.
     */
    fun list(): List<String> {
        val manager = runCatching {
            appContext.getSystemService(Context.USB_SERVICE) as? UsbManager
        }.getOrNull() ?: return emptyList()

        // getDeviceList() can throw on a few OEM builds when the USB HAL is
        // in a bad state. A diagnostic screen that crashes while diagnosing is
        // worse than one that reports nothing.
        val devices = runCatching { manager.deviceList.values.toList() }.getOrElse { emptyList() }
        return devices.map(::describe).sorted()
    }

    private fun describe(device: UsbDevice): String {
        val name = device.productName?.takeIf { it.isNotBlank() }
            ?: device.deviceName.substringAfterLast('/')
        val vendor = device.manufacturerName?.takeIf { it.isNotBlank() }
        val ids = "%04x:%04x".format(device.vendorId, device.productId)
        return if (vendor != null) "$name — $vendor ($ids)" else "$name ($ids)"
    }
}
