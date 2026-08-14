// jni_shared.h — JNI globals shared between scanengine_jni.cpp (the live
// C-ABI path) and replay_jni.cpp (B4's replay path).
//
// Both files need the same three cached classes/ctors (NativeDeviceHealth,
// NativePointPage) and the same "attach this native thread to the JVM"
// helper. JNI_OnLoad may only be defined once per .so, so scanengine_jni.cpp
// keeps sole ownership of it and does the one-time FindClass/GetMethodID
// work for all of it (see its JNI_OnLoad); this header just lets
// replay_jni.cpp see the results instead of re-resolving (and leaking a
// second set of global refs for) the same two classes.
#ifndef LIDARSCAN_JNI_SHARED_H
#define LIDARSCAN_JNI_SHARED_H

#include <jni.h>

namespace lidarscan_jni {

extern JavaVM* g_jvm;

extern jclass g_health_class;      // com.lidarscan.app.engine.NativeDeviceHealth
extern jmethodID g_health_ctor;    // (IIIIJJJJJDDDJ)V

extern jclass g_point_page_class;  // com.lidarscan.app.engine.NativePointPage
extern jmethodID g_point_page_ctor;  // (IIIIJJFFFFFFLjava/nio/ByteBuffer;)V

extern jclass g_replay_stats_class;    // com.lidarscan.app.engine.NativeReplayStats
extern jmethodID g_replay_stats_ctor;  // (JJIIZZI)V

// Attaches the calling native thread to the JVM if it is not already
// attached (*did_attach reports which happened, so the caller knows whether
// it is responsible for DetachCurrentThread once done). Never detach a
// thread this returned did_attach=false for.
JNIEnv* AttachCurrentThreadOrGet(bool* did_attach);

}  // namespace lidarscan_jni

#endif  // LIDARSCAN_JNI_SHARED_H
