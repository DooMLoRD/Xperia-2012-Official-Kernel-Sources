/*
 * Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "WebGLView.h"

#include "JNIUtility.h"
#include "WebCoreJni.h"

#include <jni.h>
#include <JNIHelp.h>
#include <surfaceflinger/Surface.h>

#include <utils/Log.h>

namespace android {

static jfieldID g_nativeClass = 0;

/*
 * Native JNI method, called from Java:
 */
static void nativeSurfaceCreated(JNIEnv* env, jobject obj)
{
    WebGLView* viewImpl = g_nativeClass ? (WebGLView*)env->GetIntField(obj, g_nativeClass) : 0;

    if (viewImpl) {
        viewImpl->surfaceCreated();
    }
}

/*
 * Native JNI method, called from Java:
 */
static void nativeSurfaceDestroyed(JNIEnv *env, jobject obj)
{
    WebGLView* viewImpl = g_nativeClass ? (WebGLView*)env->GetIntField(obj, g_nativeClass) : 0;

    if (viewImpl) {
        viewImpl->surfaceDestroyed();
    }
}

static JNINativeMethod gJavaWebViewCoreMethods[] = {
    { "nativeSurfaceCreated", "()V", (void*)nativeSurfaceCreated},
    { "nativeSurfaceDestroyed", "()V", (void*)nativeSurfaceDestroyed}
};


int register_webglview(JNIEnv* env)
{
    return jniRegisterNativeMethods(env, "android/webkit/WebGLView", gJavaWebViewCoreMethods, 2);
}

WebGLView::WebGLView(JNIEnv* env, jobject webGLViewObject)
    : m_callbackObj(0)
{
    jclass clazz = env->FindClass("android/webkit/WebGLView");
    m_obj = env->NewGlobalRef(webGLViewObject);
    g_nativeClass = env->GetFieldID(clazz, "mNativeClass", "I");
    m_getSurface = env->GetMethodID(clazz, "getSurface", "()Landroid/view/Surface;");

    /* Store a pointer to this native object in the Java object: */
    env->SetIntField(webGLViewObject, g_nativeClass, (jint)this);
}

WebGLView::~WebGLView()
{
    if (m_obj) {
        JNIEnv* env = JSC::Bindings::getJNIEnv();
        env->SetIntField(m_obj, g_nativeClass, 0);
        env->DeleteGlobalRef(m_obj);
        m_obj = 0;
    }
}

void* WebGLView::getSurface()
{
    JNIEnv* env = JSC::Bindings::getJNIEnv();
    AutoJObject obj = getRealObject(env, m_obj);

    jobject surfaceObj = env->CallObjectMethod(obj.get(), m_getSurface);
    if (!surfaceObj) {
        return 0;
    }

    jclass surfaceClass = env->FindClass("android/view/Surface");
    jfieldID surfaceFieldID = env->GetFieldID(surfaceClass, ANDROID_VIEW_SURFACE_JNI_ID, "I");
    void* window = (void*)env->GetIntField(surfaceObj, surfaceFieldID);

    return window;
}

void WebGLView::registerCallbackObject(WebGLViewCallback *callback)
{
    m_callbackObj = callback;
}

void WebGLView::surfaceCreated()
{
    if (m_callbackObj) {
        m_callbackObj->surfaceCreated();
    }
}

void WebGLView::WebGLView::surfaceDestroyed()
{
    if (m_callbackObj) {
        m_callbackObj->surfaceDestroyed();
    }
}
}
