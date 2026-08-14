package com.lidarscan.app.ui.theme

import androidx.compose.ui.graphics.Color

/**
 * Brand tokens — a neutral placeholder palette (the Tech Spec doesn't define
 * product colors yet). Kept in this one file deliberately so swapping in the
 * real brand palette later is a small, obvious diff. Everything else in the
 * app should reference [MaterialTheme.colorScheme] roles, not these values
 * directly, except for the couple of fixed semantic uses below.
 */
val BrandPrimary = Color(0xFF3C6E71) // deep teal
val BrandSecondary = Color(0xFF95C0C2) // pale teal
val BrandTertiary = Color(0xFFF4A261) // warm amber accent
val BrandError = Color(0xFFBA1A1A)

/**
 * Sensor badge colors (Projects list, and later the capture status strip):
 * fixed regardless of theme/dynamic-color so the D6-vs-Mid-360 badge stays
 * recognizable at a glance across projects.
 */
val SensorD6Badge = Color(0xFF5C6BC0)
val SensorMid360Badge = Color(0xFF00897B)
