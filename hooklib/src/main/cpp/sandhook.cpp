#include <jni.h>
#include "includes/cast_art_method.h"
#include "includes/trampoline_manager.h"
#include "includes/hide_api.h"

SandHook::TrampolineManager trampolineManager;

extern "C" int SDK_INT = 0;

enum HookMode {
    AUTO = 0,
    INLINE = 1,
    REPLACE = 2
};

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_swift_sandhook_SandHook_initNative(JNIEnv *env, jclass type, jint sdk) {

    // TODO
    SDK_INT = sdk;
    SandHook::CastArtMethod::init(env);
    trampolineManager.init(SandHook::CastArtMethod::entryPointQuickCompiled->getOffset());
    initHideApi(env, sdk);
    return JNI_TRUE;

}

void ensureMethodCached(art::mirror::ArtMethod *hookMethod, art::mirror::ArtMethod *backupMethod) {
    if (SDK_INT >= ANDROID_P)
        return;
    uint32_t index = backupMethod->getDexMethodIndex();
    if (SDK_INT < ANDROID_O2) {
        hookMethod->setDexCacheResolveItem(index, backupMethod);
    } else {
        int cacheSize = 1024;
        Size slotIndex = index % cacheSize;
        Size newCachedMethodsArray = reinterpret_cast<Size>(calloc(cacheSize, BYTE_POINT * 2));
        unsigned int one = 1;
        memcpy(reinterpret_cast<void *>(newCachedMethodsArray + BYTE_POINT), &one, 4);
        memcpy(reinterpret_cast<void *>(newCachedMethodsArray + BYTE_POINT * 2 * slotIndex),
               (&backupMethod),
               BYTE_POINT
        );
        memcpy(reinterpret_cast<void *>(newCachedMethodsArray + BYTE_POINT * 2 * slotIndex + BYTE_POINT),
               &index,
               4
        );
        hookMethod->setDexCacheResolveList(&newCachedMethodsArray);
    }
}

bool doHookWithReplacement(JNIEnv* env,
                           art::mirror::ArtMethod *originMethod,
                           art::mirror::ArtMethod *hookMethod,
                           art::mirror::ArtMethod *backupMethod) {
    if (!hookMethod->isCompiled()) {
        hookMethod->compile(env);
    }
    if (SDK_INT >= ANDROID_N) {
        originMethod->disableCompilable();
        hookMethod->disableCompilable();
    }
    originMethod->tryDisableInline();
    if (backupMethod != nullptr) {
        originMethod->backup(backupMethod);
        if (SDK_INT >= ANDROID_N) {
            backupMethod->disableCompilable();
        }
        if (SDK_INT >= ANDROID_O) {
            backupMethod->disableInterpreterForO();
        }
        backupMethod->tryDisableInline();
        backupMethod->setPrivate();
        backupMethod->flushCache();
    }

    if (SDK_INT >= ANDROID_O) {
        originMethod->disableInterpreterForO();
    }
    SandHook::HookTrampoline* hookTrampoline = trampolineManager.installReplacementTrampoline(originMethod, hookMethod, backupMethod);
    if (hookTrampoline != nullptr) {
        originMethod->setQuickCodeEntry(hookTrampoline->replacement->getCode());
        void* entryPointFormInterpreter = hookMethod->getInterpreterCodeEntry();
        if (entryPointFormInterpreter != NULL) {
            originMethod->setInterpreterCodeEntry(entryPointFormInterpreter);
        }
        if (hookTrampoline->callOrigin != nullptr) {
            backupMethod->setQuickCodeEntry(hookTrampoline->callOrigin->getCode());
            backupMethod->flushCache();
        }
        originMethod->flushCache();
        return true;
    } else {
        return false;
    }
}

bool doHookWithInline(JNIEnv* env,
                      art::mirror::ArtMethod *originMethod,
                      art::mirror::ArtMethod *hookMethod,
                      art::mirror::ArtMethod *backupMethod) {

    //fix >= 8.1
    if (!hookMethod->isCompiled()) {
        hookMethod->compile(env);
    }

    if (SDK_INT >= ANDROID_N) {
        originMethod->disableCompilable();
    }
    originMethod->tryDisableInline();
    originMethod->flushCache();

    SandHook::HookTrampoline* hookTrampoline = trampolineManager.installInlineTrampoline(originMethod, hookMethod, backupMethod);

    if (hookTrampoline == nullptr)
        return false;

    hookMethod->flushCache();
    if (hookTrampoline->callOrigin != nullptr) {
        //backup
        originMethod->backup(backupMethod);
        backupMethod->setQuickCodeEntry(hookTrampoline->callOrigin->getCode());

        if (SDK_INT >= ANDROID_N) {
            backupMethod->disableCompilable();
        }
        if (SDK_INT >= ANDROID_O) {
            backupMethod->disableInterpreterForO();
        }
        backupMethod->tryDisableInline();
        backupMethod->setPrivate();
        backupMethod->flushCache();
    }
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_swift_sandhook_SandHook_hookMethod(JNIEnv *env, jclass type, jobject originMethod,
                                            jobject hookMethod, jobject backupMethod, jint hookMode) {

    // TODO
    art::mirror::ArtMethod* origin = reinterpret_cast<art::mirror::ArtMethod *>(env->FromReflectedMethod(originMethod));
    art::mirror::ArtMethod* hook = reinterpret_cast<art::mirror::ArtMethod *>(env->FromReflectedMethod(hookMethod));
    art::mirror::ArtMethod* backup = backupMethod == NULL ? nullptr : reinterpret_cast<art::mirror::ArtMethod *>(env->FromReflectedMethod(backupMethod));

    bool isInlineHook = false;

    int mode = reinterpret_cast<int>(hookMode);

    if (mode == INLINE) {
        if (!origin->isCompiled()) {
            if (SDK_INT >= ANDROID_N) {
                isInlineHook = origin->compile(env);
            }
        } else {
            isInlineHook = true;
        }
        goto label_hook;
    } else if (mode == REPLACE) {
        isInlineHook = false;
        goto label_hook;
    }


    if (origin->isAbstract()) {
        isInlineHook = false;
    } else if (!origin->isCompiled()) {
        if (SDK_INT >= ANDROID_N) {
            if (SDK_INT >= ANDROID_N) {
                isInlineHook = origin->compile(env);
            }
        } else {
            isInlineHook = false;
        }
    } else {
        isInlineHook = true;
    }


label_hook:
    if (isInlineHook && trampolineManager.canSafeInline(origin)) {
        return static_cast<jboolean>(doHookWithInline(env, origin, hook, backup));
    } else {
        return static_cast<jboolean>(doHookWithReplacement(env, origin, hook, backup));
    }

}

extern "C"
JNIEXPORT void JNICALL
Java_com_swift_sandhook_SandHook_ensureMethodCached(JNIEnv *env, jclass type, jobject hook,
                                                    jobject backup) {
    art::mirror::ArtMethod* hookeMethod = reinterpret_cast<art::mirror::ArtMethod *>(env->FromReflectedMethod(hook));
    art::mirror::ArtMethod* backupMethod = backup == NULL ? nullptr : reinterpret_cast<art::mirror::ArtMethod *>(env->FromReflectedMethod(backup));
    ensureMethodCached(hookeMethod, backupMethod);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_swift_sandhook_test_TestClass_jni_1test(JNIEnv *env, jobject instance) {
    int a = 1 + 1;
    int b = a + 1;
}
extern "C"
JNIEXPORT void JNICALL
Java_com_swift_sandhook_ClassNeverCall_neverCallNative(JNIEnv *env, jobject instance) {

    // TODO
    int a = 1 + 1;
    int b = a + 1;

}