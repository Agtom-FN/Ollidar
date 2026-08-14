# B1 scaffold ships with minification disabled (see build.gradle.kts).
# Rules below are placeholders for when a later workstream (B6+, JNI in A1)
# turns minification on for release builds.

# Keep kotlinx.serialization models used for manifest.json round-tripping.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.AnnotationsKt
-keepclassmembers class com.lidarscan.core.** {
    *** Companion;
}
-keepclasseswithmembers class com.lidarscan.core.** {
    kotlinx.serialization.KSerializer serializer(...);
}
