// QAuxiliary - An Xposed module for QQ/TIM
// Copyright (C) 2019-2023 QAuxiliary developers
// https://github.com/cinit/QAuxiliary
//
// This software is non-free but opensource software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General Public License
// as published by the Free Software Foundation; either
// version 3 of the License, or any later version and our eula as published
// by QAuxiliary contributors.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// and eula along with this software.  If not, see
// <https://www.gnu.org/licenses/>
// <https://github.com/cinit/QAuxiliary/blob/master/LICENSE.md>.

//
// Created by sulfate on 2023-05-18.
//

#include "NativeCoreBridge.h"

#include <jni.h>
#include <string>
#include <mutex>
#include <vector>
#include <algorithm>
#include <optional>
#include <unistd.h>
#include <cstring>
#include <cerrno>


#include "HostInfo.h"
#include "utils/JniUtils.h"
#include "utils/Log.h"
#include "dobby.h"
#include "utils/FileMemMap.h"
#include "utils/ProcessView.h"
#include "utils/ElfView.h"
#include "utils/ConfigManager.h"

#include "natives_utils.h"


struct NativeHookHandle {
    int (* hookFunction)(void* func, void* replace, void** backup);
    int (* unhookFunction)(void* func);
};

static NativeHookHandle sNativeHookHandle = {};

namespace qauxv {

void HandleLoadLibrary(const char* name, void* handle);

}


namespace qauxv {

std::vector<LoadLibraryCallback> sCallbacks;

std::mutex sCallbacksMutex;
bool sHandleLoadLibraryCallbackInitialized = false;

void HandleLoadLibrary(const char* name, void* handle) {
    std::vector<LoadLibraryCallback> callbacks;
    {
        std::scoped_lock lock(sCallbacksMutex);
        callbacks = sCallbacks;
    }
    for (const auto& callback: callbacks) {
        callback(name, handle);
    }
}

int RegisterLoadLibraryCallback(const LoadLibraryCallback& callback) {
    if (!sHandleLoadLibraryCallbackInitialized) {
        return -1;
    }
    std::scoped_lock lock(sCallbacksMutex);
    sCallbacks.push_back(callback);
    return 0;
}

static volatile bool sIsNativeInitialized = false;

int CreateInlineHook(void* func, void* replace, void** backup) {
    if (!sIsNativeInitialized) {
        LOGE("CreateInlineHook: native core is not initialized");
        return RS_FAILED;
    }
    return sNativeHookHandle.hookFunction(func, replace, backup);
}

int DestroyInlineHook(void* func) {
    if (!sIsNativeInitialized) {
        LOGE("DestroyInlineHook: native core is not initialized");
        return RS_FAILED;
    }
    return sNativeHookHandle.unhookFunction(func);
}

void* backup_do_dlopen = nullptr;

void* fake_do_dlopen_24(const char* name, int flags, const void* extinfo, const void* caller) {
    auto* backup = (void* (*)(const char* name, int flags, const void* extinfo, const void* caller)) backup_do_dlopen;
    auto handle = backup(name, flags, extinfo, caller);
    HandleLoadLibrary(name, handle);
    return handle;
}

void* fake_do_dlopen_23(const char* name, int flags, const void* extinfo) {
    // below Android 7.0, the caller address is not passed to do_dlopen,
    // because there is no linker namespace concept before Android 7.0
    auto* backup = (void* (*)(const char* name, int flags, const void* extinfo)) backup_do_dlopen;
    auto handle = backup(name, flags, extinfo);
    HandleLoadLibrary(name, handle);
    return handle;
}

void HookLoadLibrary() {
    using namespace utils;
    LOGD("HookLoadLibrary: attempting to hook ld-android.so!__dl__Z9do_dlopenPKciPK17android_dlextinfoPKv");
    const char* soname;
    if constexpr (sizeof(void*) == 8) {
        soname = "linker64";
    } else {
        soname = "linker";
    }
    bool isBelow24 = false;
    // __dl__Z9do_dlopenPKciPK17android_dlextinfoPKv, Android 8.0+
    void* symbol = DobbySymbolResolver(soname, "__dl__Z9do_dlopenPKciPK17android_dlextinfoPKv");
    if (symbol == nullptr) {
        // __dl__Z9do_dlopenPKciPK17android_dlextinfoPv, Android 7.x
        symbol = DobbySymbolResolver(soname, "__dl__Z9do_dlopenPKciPK17android_dlextinfoPv");
    }
    if (symbol == nullptr) {
        // __dl__Z9do_dlopenPKciPK17android_dlextinfo, Android 6.x
        symbol = DobbySymbolResolver(soname, "__dl__Z9do_dlopenPKciPK17android_dlextinfo");
        if (symbol != nullptr) {
            isBelow24 = true;
        }
    }
    if (symbol == nullptr) {
        // give up
        LOGE("HookLoadLibrary: failed to find __dl__Z9do_dlopenPKciPK17android_dlextinfoPKv");
        return;
    }
    auto hookHandler = isBelow24 ? (dobby_dummy_func_t) fake_do_dlopen_23 : (dobby_dummy_func_t) fake_do_dlopen_24;
    if (DobbyHook(symbol, hookHandler, (dobby_dummy_func_t*) &qauxv::backup_do_dlopen) != RS_SUCCESS) {
        LOGE("HookLoadLibrary: failed to hook __dl__Z9do_dlopenPKciPK17android_dlextinfoPKv");
        return;
    }
    sHandleLoadLibraryCallbackInitialized = true;
    LOGD("HookLoadLibrary: hooked __dl__Z9do_dlopenPKciPK17android_dlextinfoPKv");
}

void TraceError(JNIEnv* env, jobject thiz, std::string_view errMsg) {
    bool isAttachedManually = false;
    if (thiz == nullptr) {
        LOGE("TraceError fatal thiz == null");
        return;
    }
    if (errMsg.empty()) {
        LOGE("TraceError fatal errMsg == null");
        return;
    }
    if (env == nullptr) {
        JavaVM* vm = HostInfo::GetJavaVM();
        if (vm == nullptr) {
            LOGE("TraceError fatal vm == null");
            return;
        }
        // check if current thread is attached to jvm
        jint err = vm->GetEnv((void**) &env, JNI_VERSION_1_6);
        if (err == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                LOGE("TraceError fatal AttachCurrentThread failed");
                return;
            }
            isAttachedManually = true;
        } else if (env == nullptr) {
            LOGE("TraceError fatal GetEnv failed, err = {}", err);
            return;
        }
    }
    const auto fnDetachCurrentThread = [isAttachedManually, env]() {
        if (isAttachedManually) {
            JavaVM* vm = nullptr;
            if (env->GetJavaVM(&vm) != JNI_OK) {
                LOGE("TraceError fatal GetJavaVM failed");
                return;
            }
            if (vm->DetachCurrentThread() != JNI_OK) {
                LOGE("TraceError fatal DetachCurrentThread failed");
                return;
            }
        }
    };
    // this method is typically not called frequently, so we don't need to care about performance
    if (env->PushLocalFrame(16) != JNI_OK) {
        LOGE("TraceError fatal PushLocalFrame failed");
        env->ExceptionDescribe();
        env->ExceptionClear();
        fnDetachCurrentThread();
        return;
    }
    const auto fnPopLocalFrame = [env]() {
        if (env->PopLocalFrame(nullptr) != JNI_OK) {
            LOGE("TraceError fatal PopLocalFrame failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    };
    const auto fnGetClassLoader = [env, thiz]() -> jobject {
        jclass clazz = env->GetObjectClass(thiz);
        if (clazz == nullptr) {
            LOGE("TraceError fatal GetObjectClass failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return nullptr;
        }
        jclass kClass = env->FindClass("java/lang/Class");
        if (kClass == nullptr) {
            LOGE("TraceError fatal GetObjectClass failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return nullptr;
        }
        jmethodID getClassLoaderMethod = env->GetMethodID(kClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
        if (getClassLoaderMethod == nullptr) {
            LOGE("TraceError fatal GetMethodID getClassLoader failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return nullptr;
        }
        jobject classLoader = env->CallObjectMethod(clazz, getClassLoaderMethod);
        if (classLoader == nullptr) {
            LOGE("TraceError fatal CallObjectMethod getClassLoader failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return nullptr;
        }
        return classLoader;
    };
    const auto fnCallTraceErrorMethod = [env, thiz, errMsg](jobject classloader) {
        jclass kClassLoader = env->FindClass("java/lang/ClassLoader");
        jmethodID loadClassMethod = env->GetMethodID(kClassLoader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        jclass kClass = (jclass) env->CallObjectMethod(classloader, loadClassMethod, env->NewStringUTF("io.github.qauxv.core.NativeCoreBridge"));
        if (kClass == nullptr) {
            LOGE("TraceError fatal CallObjectMethod loadClass NativeCoreBridge failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }
        jmethodID nativeTraceErrorHelperMethod = env->GetStaticMethodID(kClass, "nativeTraceErrorHelper", "(Ljava/lang/Object;Ljava/lang/Throwable;)V");
        if (nativeTraceErrorHelperMethod == nullptr) {
            LOGE("TraceError fatal GetStaticMethodID nativeTraceErrorHelper failed");
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }
        jstring errMsgJString = env->NewStringUTF(errMsg.data());
        if (errMsgJString == nullptr) {
            LOGE("TraceError fatal NewStringUTF failed, original error message: {}", errMsg);
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }
        auto th = (jthrowable) env->NewObject(env->FindClass("java/lang/RuntimeException"),
                                              env->GetMethodID(env->FindClass("java/lang/RuntimeException"),
                                                               "<init>", "(Ljava/lang/String;)V"),
                                              errMsgJString);
        env->CallStaticVoidMethod(kClass, nativeTraceErrorHelperMethod, thiz, th);
        if (env->ExceptionCheck()) {
            LOGE("TraceError fatal CallStaticVoidMethod nativeTraceErrorHelper failed, original error message: {}", errMsg);
            env->ExceptionDescribe();
            env->ExceptionClear();
            return;
        }
    };
    auto classLoader = fnGetClassLoader();
    if (classLoader != nullptr) {
        fnCallTraceErrorMethod(classLoader);
    }
    fnPopLocalFrame();
    fnDetachCurrentThread();
}

}

// called by Xposed framework
EXPORT extern "C" [[maybe_unused]] NativeOnModuleLoaded native_init(const NativeAPIEntries* entries) {
    sNativeHookHandle.hookFunction = entries->hookFunc;
    sNativeHookHandle.unhookFunction = entries->unhookFunc;
    qauxv::sHandleLoadLibraryCallbackInitialized = true;
    return &qauxv::HandleLoadLibrary;
}


extern "C" JNIEXPORT void JNICALL
Java_io_github_qauxv_core_NativeCoreBridge_initNativeCore(JNIEnv* env,
                                                          jclass,
                                                          jstring package_name,
                                                          jint current_sdk_level,
                                                          jstring version_name,
                                                          jlong long_version_code) {
    using namespace qauxv;
    if (sIsNativeInitialized) {
        LOGE("initNativeCore: native core is already initialized");
        return;
    }
    auto packageName = JstringToString(env, package_name);
    auto versionName = JstringToString(env, version_name);
    if (!packageName.has_value() || !versionName.has_value()) {
        LOGE("initNativeCore: failed to get package name or version name");
        return;
    }
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        LOGE("initNativeCore: failed to get JavaVM");
        return;
    }
    HostInfo::InitHostInfo(current_sdk_level, packageName.value(),
                           versionName.value(), uint64_t(long_version_code), vm);
    if (sNativeHookHandle.hookFunction == nullptr) {
        LOGD("initNativeCore: native hook function is null, Dobby will be used");
        sNativeHookHandle.hookFunction = +[](void* func, void* replace, void** backup) {
            return DobbyHook((void*) func, (dobby_dummy_func_t) replace, (dobby_dummy_func_t*) backup);
        };
        sNativeHookHandle.unhookFunction = +[](void* func) {
            return DobbyDestroy((void*) func);
        };
        if (!sHandleLoadLibraryCallbackInitialized) {
            HookLoadLibrary();
        }
    } else {
        LOGD("initNativeCore: native hook function is not null, use it directly");
    }
    sIsNativeInitialized = true;
}
