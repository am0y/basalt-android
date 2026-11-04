#include "device_http.h"
#include "sha384.h"

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_HTTP_BODY (8u * 1024u * 1024u)
#define READ_CHUNK 65536

static JavaVM* java_vm;
static jclass isolated_http_class;
static jmethodID isolated_http_execute;
static jclass editor_input_class;
static jmethodID editor_input_exchange;
static jclass input_class;
static jmethodID input_send;
static jmethodID input_foreground;
static jmethodID input_attach;
static int input_attached;

int device_fingerprint(char output[97], char* error, size_t error_size)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static char cached[97];
    pthread_mutex_lock(&mutex);
    if (cached[0]) {
        memcpy(output, cached, sizeof(cached));
        pthread_mutex_unlock(&mutex);
        return 0;
    }
    int result = -1, attached = 0;
    JNIEnv* env = NULL;
    if (!java_vm) goto unavailable;
    attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) goto unavailable;
    if (!env) goto unavailable;
    if ((*env)->PushLocalFrame(env, 16) < 0) goto done;
    jclass activity = (*env)->FindClass(env, "android/app/ActivityThread");
    jclass context = (*env)->FindClass(env, "android/content/Context");
    jclass settings = (*env)->FindClass(env, "android/provider/Settings$Secure");
    if (!activity || !context || !settings) goto frame_done;
    jmethodID current = (*env)->GetStaticMethodID(env, activity, "currentApplication", "()Landroid/app/Application;");
    jmethodID resolver = (*env)->GetMethodID(env, context, "getContentResolver", "()Landroid/content/ContentResolver;");
    jmethodID get = (*env)->GetStaticMethodID(env, settings, "getString", "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    if (!current || !resolver || !get) goto frame_done;
    jobject app = (*env)->CallStaticObjectMethod(env, activity, current);
    if (!app || (*env)->ExceptionCheck(env)) goto frame_done;
    jobject content = (*env)->CallObjectMethod(env, app, resolver);
    jstring key = (*env)->NewStringUTF(env, "android_id");
    if (!content || !key || (*env)->ExceptionCheck(env)) goto frame_done;
    jstring id = (*env)->CallStaticObjectMethod(env, settings, get, content, key);
    if (!id || (*env)->ExceptionCheck(env)) goto frame_done;
    const char* value = (*env)->GetStringUTFChars(env, id, NULL);
    if (!value) goto frame_done;
    if (strlen(value) == 16 && strspn(value, "0123456789abcdefABCDEF") == 16 &&
        strcmp(value, "0000000000000000") && strcasecmp(value, "9774d56d682e549c")) {
        char normalized[16];
        for (unsigned i = 0; i < 16; ++i)
            normalized[i] = value[i] >= 'A' && value[i] <= 'F' ? value[i] + ('a' - 'A') : value[i];
        Sha384 hash;
        static const char domain[] = "Basalt/Android/device/v1";
        sha384_init(&hash);
        sha384_update(&hash, domain, sizeof(domain));
        sha384_update(&hash, normalized, sizeof(normalized));
        sha384_finish(&hash, cached);
        memcpy(output, cached, sizeof(cached));
        result = 0;
    }
    (*env)->ReleaseStringUTFChars(env, id, value);
frame_done:
    (*env)->PopLocalFrame(env, NULL);
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
unavailable:
    if (result) snprintf(error, error_size, "Android device identifier unavailable");
    pthread_mutex_unlock(&mutex);
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    (void)reserved;
    java_vm = vm;
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass type = (*env)->FindClass(env, "com/roblox/client/startup/BasaltHttp");
    if (type) isolated_http_class = (*env)->NewGlobalRef(env, type);
    if (isolated_http_class)
        isolated_http_execute = (*env)->GetStaticMethodID(env, isolated_http_class, "execute", "([B)[B");
    if (!isolated_http_execute || (*env)->ExceptionCheck(env))
    {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return JNI_ERR;
    }
    type = (*env)->FindClass(env, "com/roblox/client/startup/BasaltEditorInput");
    if (type) editor_input_class = (*env)->NewGlobalRef(env, type);
    if (editor_input_class)
        editor_input_exchange = (*env)->GetStaticMethodID(env, editor_input_class, "exchange", "(FFFFFFZ)[F");
    if (!editor_input_exchange || (*env)->ExceptionCheck(env))
    {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return JNI_ERR;
    }
    type = (*env)->FindClass(env, "com/roblox/client/startup/BasaltInputSimulation");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (type) input_class = (*env)->NewGlobalRef(env, type);
    if (input_class) {
        input_send = (*env)->GetStaticMethodID(env, input_class, "send", "(IIFFFF)Z");
        input_foreground = (*env)->GetStaticMethodID(env, input_class, "foreground", "()Z");
        input_attach = (*env)->GetStaticMethodID(env, input_class, "attach", "(Landroid/app/Activity;)V");
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    return JNI_VERSION_1_6;
}

static void java_error(JNIEnv* env, char* error, size_t size)
{
    jthrowable thrown = (*env)->ExceptionOccurred(env);
    if (!thrown)
    {
        snprintf(error, size, "Android HTTP operation failed");
        return;
    }
    (*env)->ExceptionClear(env);
    jclass throwable_class = (*env)->FindClass(env, "java/lang/Throwable");
    if (!throwable_class) goto generic;
    jmethodID to_string = (*env)->GetMethodID(env, throwable_class, "toString", "()Ljava/lang/String;");
    if (!to_string) goto generic;
    jstring message = (jstring)(*env)->CallObjectMethod(env, thrown, to_string);
    if (!message || (*env)->ExceptionCheck(env)) goto generic;
    const char* text = (*env)->GetStringUTFChars(env, message, NULL);
    if (!text) goto generic;
    snprintf(error, size, "%s", text);
    (*env)->ReleaseStringUTFChars(env, message, text);
    return;
generic:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    snprintf(error, size, "Android HTTP exception");
}

// Locate the live Roblox activity through ActivityThread so input simulation
// can attach itself without any DEX-side startup hook.
static void bind_input_activity(JNIEnv* env)
{
    if (input_attached || !input_class || !input_attach) return;
    jclass thread_class = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!thread_class) { (*env)->ExceptionClear(env); return; }
    jmethodID current = (*env)->GetStaticMethodID(env, thread_class, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject thread = current ? (*env)->CallStaticObjectMethod(env, thread_class, current) : NULL;
    if (!thread || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
    jfieldID activities_field = (*env)->GetFieldID(env, thread_class, "mActivities", "Landroid/util/ArrayMap;");
    jobject activities = activities_field ? (*env)->GetObjectField(env, thread, activities_field) : NULL;
    if (!activities || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
    jclass map_class = (*env)->FindClass(env, "android/util/ArrayMap");
    jmethodID size_method = map_class ? (*env)->GetMethodID(env, map_class, "size", "()I") : NULL;
    jmethodID value_at = map_class ? (*env)->GetMethodID(env, map_class, "valueAt", "(I)Ljava/lang/Object;") : NULL;
    jclass record_class = (*env)->FindClass(env, "android/app/ActivityThread$ActivityClientRecord");
    jfieldID activity_field = record_class ? (*env)->GetFieldID(env, record_class, "activity", "Landroid/app/Activity;") : NULL;
    if (!size_method || !value_at || !activity_field)
    { (*env)->ExceptionClear(env); return; }
    jint size = (*env)->CallIntMethod(env, activities, size_method);
    for (jint i = 0; i < size; ++i)
    {
        jobject record = (*env)->CallObjectMethod(env, activities, value_at, i);
        if (!record || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); continue; }
        jobject activity = (*env)->GetObjectField(env, record, activity_field);
        if (!activity || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); continue; }
        (*env)->CallStaticVoidMethod(env, input_class, input_attach, activity);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
        input_attached = 1;
        return;
    }
    (*env)->ExceptionClear(env);
}

int device_input_event(int kind, int code, float x, float y, float width, float height,
                       char* error, size_t error_size)
{
    if (!java_vm || !input_send) { snprintf(error, error_size, "Android input helper unavailable"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    { snprintf(error, error_size, "could not attach input thread"); return -1; }
    if (!input_attached) bind_input_activity(env);
    jboolean accepted = (*env)->CallStaticBooleanMethod(env, input_class, input_send,
        (jint)kind, (jint)code, (jfloat)x, (jfloat)y, (jfloat)width, (jfloat)height);
    int failed = (*env)->ExceptionCheck(env);
    if (failed) java_error(env, error, error_size);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    if (failed) return -1;
    if (!accepted) { snprintf(error, error_size, "Roblox game view is not active or input is unsupported"); return -1; }
    return 0;
}

int device_input_foreground(void)
{
    if (!java_vm || !input_foreground) return 0;
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) return 0;
    int result = (*env)->CallStaticBooleanMethod(env, input_class, input_foreground) &&
        !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}

static int valid_utf8(const uint8_t* bytes, size_t length)
{
    for (size_t i = 0; i < length;)
    {
        unsigned c = bytes[i++];
        if (c < 0x80) continue;
        unsigned count = c >= 0xc2 && c <= 0xdf ? 1 :
                         c >= 0xe0 && c <= 0xef ? 2 :
                         c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!count || count > length - i) return 0;
        unsigned first = bytes[i];
        if ((first & 0xc0) != 0x80 ||
            (c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first > 0x8f)) return 0;
        for (unsigned n = 0; n < count; ++n)
            if ((bytes[i++] & 0xc0) != 0x80) return 0;
    }
    return 1;
}

int device_editor_input(const float bounds[6], int enabled, float state[23], char* error, size_t error_size)
{
    if (!java_vm || !editor_input_exchange) { snprintf(error, error_size, "editor input bridge unavailable"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) return -1;
    if (!env) return -1;
    jvalue args[7];
    for (int i = 0; i < 6; ++i) args[i].f = bounds[i];
    args[6].z = enabled != 0;
    jfloatArray result = (jfloatArray)(*env)->CallStaticObjectMethodA(env, editor_input_class, editor_input_exchange, args);
    int ok = result && !(*env)->ExceptionCheck(env) && (*env)->GetArrayLength(env, result) == 23;
    if (ok) (*env)->GetFloatArrayRegion(env, result, 0, 23, state);
    if ((*env)->ExceptionCheck(env)) { java_error(env, error, error_size); ok = 0; }
    if (result) (*env)->DeleteLocalRef(env, result);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return ok ? 0 : -1;
}

int device_setclipboard(const char* text, size_t length, char* error, size_t error_size)
{
    if (!java_vm || length > 1024u * 1024u || !valid_utf8((const uint8_t*)text, length))
    { snprintf(error, error_size, "clipboard expects UTF-8 text of at most 1 MiB"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    { snprintf(error, error_size, "could not attach clipboard thread"); return -1; }
    if (!env) { snprintf(error, error_size, "Android clipboard runtime unavailable"); return -1; }
    int result = -1;
    if ((*env)->PushLocalFrame(env, 24) < 0)
    {
        java_error(env, error, error_size);
        if (attached) (*java_vm)->DetachCurrentThread(java_vm);
        return -1;
    }
    jclass activity = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!activity) goto clipboard_done;
    jmethodID current = (*env)->GetStaticMethodID(env, activity, "currentApplication", "()Landroid/app/Application;");
    if (!current) goto clipboard_done;
    jobject application = (*env)->CallStaticObjectMethod(env, activity, current);
    if (!application || (*env)->ExceptionCheck(env)) goto clipboard_done;
    jclass context = (*env)->FindClass(env, "android/content/Context");
    if (!context) goto clipboard_done;
    jmethodID service = (*env)->GetMethodID(env, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring clipboard_name = (*env)->NewStringUTF(env, "clipboard");
    if (!service || !clipboard_name) goto clipboard_done;
    jobject manager = (*env)->CallObjectMethod(env, application, service, clipboard_name);
    if (!manager || (*env)->ExceptionCheck(env)) goto clipboard_done;
    jclass strings = (*env)->FindClass(env, "java/lang/String");
    jclass clip_class = (*env)->FindClass(env, "android/content/ClipData");
    jclass manager_class = (*env)->FindClass(env, "android/content/ClipboardManager");
    if (!strings || !clip_class || !manager_class) goto clipboard_done;
    jmethodID string_ctor = (*env)->GetMethodID(env, strings, "<init>", "([BLjava/lang/String;)V");
    jmethodID plain_text = (*env)->GetStaticMethodID(env, clip_class, "newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    jmethodID set_clip = (*env)->GetMethodID(env, manager_class, "setPrimaryClip", "(Landroid/content/ClipData;)V");
    if (!string_ctor || !plain_text || !set_clip) goto clipboard_done;
    jbyteArray bytes = (*env)->NewByteArray(env, (jsize)length);
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    jstring label = (*env)->NewStringUTF(env, "Basalt");
    if (!bytes || !utf8 || !label) goto clipboard_done;
    if (length) (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte*)text);
    if ((*env)->ExceptionCheck(env)) goto clipboard_done;
    jobject content = (*env)->NewObject(env, strings, string_ctor, bytes, utf8);
    if (!content || (*env)->ExceptionCheck(env)) goto clipboard_done;
    jobject clip = (*env)->CallStaticObjectMethod(env, clip_class, plain_text, label, content);
    if (!clip || (*env)->ExceptionCheck(env)) goto clipboard_done;
    (*env)->CallVoidMethod(env, manager, set_clip, clip);
    if (!(*env)->ExceptionCheck(env)) result = 0;
clipboard_done:
    if (result) java_error(env, error, error_size);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}

static jobject application_service(JNIEnv* env, const char* name, jobject* application)
{
    jclass activity = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!activity) return NULL;
    jmethodID current = (*env)->GetStaticMethodID(env, activity, "currentApplication", "()Landroid/app/Application;");
    if (!current) return NULL;
    *application = (*env)->CallStaticObjectMethod(env, activity, current);
    if (!*application || (*env)->ExceptionCheck(env)) return NULL;
    jclass context = (*env)->FindClass(env, "android/content/Context");
    if (!context) return NULL;
    jmethodID service = (*env)->GetMethodID(env, context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring key = (*env)->NewStringUTF(env, name);
    if (!service || !key) return NULL;
    return (*env)->CallObjectMethod(env, *application, service, key);
}

int device_getclipboard(char** text, size_t* length, char* error, size_t error_size)
{
    *text = NULL;
    *length = 0;
    if (!java_vm) { snprintf(error, error_size, "Android clipboard unavailable"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    { snprintf(error, error_size, "could not attach clipboard thread"); return -1; }
    if (!env) { snprintf(error, error_size, "Android clipboard unavailable"); return -1; }
    int result = -1;
    if ((*env)->PushLocalFrame(env, 32) < 0)
    {
        java_error(env, error, error_size);
        if (attached) (*java_vm)->DetachCurrentThread(java_vm);
        return -1;
    }
    jobject application = NULL;
    jobject manager = application_service(env, "clipboard", &application);
    if (!manager || (*env)->ExceptionCheck(env)) goto read_done;
    jclass manager_class = (*env)->GetObjectClass(env, manager);
    if (!manager_class) goto read_done;
    jmethodID get_clip = (*env)->GetMethodID(env, manager_class, "getPrimaryClip", "()Landroid/content/ClipData;");
    if (!get_clip) goto read_done;
    jobject clip = (*env)->CallObjectMethod(env, manager, get_clip);
    if ((*env)->ExceptionCheck(env)) goto read_done;
    if (!clip) { result = 0; goto read_done; }
    jclass clip_class = (*env)->GetObjectClass(env, clip);
    if (!clip_class) goto read_done;
    jmethodID count = (*env)->GetMethodID(env, clip_class, "getItemCount", "()I");
    jmethodID at = (*env)->GetMethodID(env, clip_class, "getItemAt", "(I)Landroid/content/ClipData$Item;");
    if (!count || !at) goto read_done;
    jint items = (*env)->CallIntMethod(env, clip, count);
    if ((*env)->ExceptionCheck(env)) goto read_done;
    if (!items) { result = 0; goto read_done; }
    jobject item = (*env)->CallObjectMethod(env, clip, at, 0);
    if (!item || (*env)->ExceptionCheck(env)) goto read_done;
    jclass item_class = (*env)->GetObjectClass(env, item);
    if (!item_class) goto read_done;
    jmethodID coerce = (*env)->GetMethodID(env, item_class, "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;");
    if (!coerce) goto read_done;
    jobject content = (*env)->CallObjectMethod(env, item, coerce, application);
    if ((*env)->ExceptionCheck(env)) goto read_done;
    if (!content) { result = 0; goto read_done; }
    jclass sequence = (*env)->FindClass(env, "java/lang/CharSequence");
    jclass strings = (*env)->FindClass(env, "java/lang/String");
    if (!sequence || !strings) goto read_done;
    jmethodID to_string = (*env)->GetMethodID(env, sequence, "toString", "()Ljava/lang/String;");
    jmethodID get_bytes = (*env)->GetMethodID(env, strings, "getBytes", "(Ljava/lang/String;)[B");
    if (!to_string || !get_bytes) goto read_done;
    jobject value = (*env)->CallObjectMethod(env, content, to_string);
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    if (!value || !utf8 || (*env)->ExceptionCheck(env)) goto read_done;
    jbyteArray bytes = (jbyteArray)(*env)->CallObjectMethod(env, value, get_bytes, utf8);
    if (!bytes || (*env)->ExceptionCheck(env)) goto read_done;
    jsize size = (*env)->GetArrayLength(env, bytes);
    if (size < 0 || size > 1024 * 1024)
    { snprintf(error, error_size, "clipboard exceeds 1 MiB"); goto read_done; }
    *text = malloc((size_t)size + 1);
    if (!*text) { snprintf(error, error_size, "clipboard allocation failed"); goto read_done; }
    if (size) (*env)->GetByteArrayRegion(env, bytes, 0, size, (jbyte*)*text);
    if ((*env)->ExceptionCheck(env)) { free(*text); *text = NULL; goto read_done; }
    (*text)[size] = 0;
    *length = (size_t)size;
    result = 0;
read_done:
    if (result && !error[0]) java_error(env, error, error_size);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}

int device_battery_percent(void)
{
    if (!java_vm) return -1;
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) return -1;
    if (!env) return -1;
    int result = -1;
    if ((*env)->PushLocalFrame(env, 16) >= 0)
    {
        jobject application = NULL;
        jobject manager = application_service(env, "batterymanager", &application);
        if (manager && !(*env)->ExceptionCheck(env))
        {
            jclass type = (*env)->GetObjectClass(env, manager);
            jmethodID property = type ? (*env)->GetMethodID(env, type, "getIntProperty", "(I)I") : NULL;
            if (property) result = (*env)->CallIntMethod(env, manager, property, 4);
        }
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); result = -1; }
        (*env)->PopLocalFrame(env, NULL);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result >= 0 && result <= 100 ? result : -1;
}

int device_request_storage_access(char* error, size_t error_size)
{
    if (!java_vm) { snprintf(error, error_size, "Android runtime unavailable"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    { snprintf(error, error_size, "could not attach storage request thread"); return -1; }
    if (!env) { snprintf(error, error_size, "Android runtime unavailable"); return -1; }
    int result = -1;
    if ((*env)->PushLocalFrame(env, 24) < 0)
    {
        java_error(env, error, error_size);
        if (attached) (*java_vm)->DetachCurrentThread(java_vm);
        return -1;
    }
    jclass activity = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!activity) goto storage_done;
    jmethodID current = (*env)->GetStaticMethodID(env, activity, "currentApplication", "()Landroid/app/Application;");
    if (!current) goto storage_done;
    jobject application = (*env)->CallStaticObjectMethod(env, activity, current);
    if (!application || (*env)->ExceptionCheck(env)) goto storage_done;
    jclass version = (*env)->FindClass(env, "android/os/Build$VERSION");
    if (!version) goto storage_done;
    jfieldID sdk_field = (*env)->GetStaticFieldID(env, version, "SDK_INT", "I");
    if (!sdk_field) goto storage_done;
    jint sdk = (*env)->GetStaticIntField(env, version, sdk_field);
    jclass context = (*env)->FindClass(env, "android/content/Context");
    if (!context) goto storage_done;
    const char* action = "android.settings.APPLICATION_DETAILS_SETTINGS";
    if (sdk >= 30)
    {
        jclass environment = (*env)->FindClass(env, "android/os/Environment");
        if (!environment) goto storage_done;
        jmethodID manager = (*env)->GetStaticMethodID(env, environment, "isExternalStorageManager", "()Z");
        if (!manager) goto storage_done;
        jboolean granted = (*env)->CallStaticBooleanMethod(env, environment, manager);
        if ((*env)->ExceptionCheck(env)) goto storage_done;
        if (granted) { result = 0; goto storage_done; }
        action = "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION";
    }
    else
    {
        jmethodID check = (*env)->GetMethodID(env, context, "checkSelfPermission", "(Ljava/lang/String;)I");
        jstring permission = (*env)->NewStringUTF(env, "android.permission.WRITE_EXTERNAL_STORAGE");
        if (!check || !permission) goto storage_done;
        jint granted = (*env)->CallIntMethod(env, application, check, permission);
        if ((*env)->ExceptionCheck(env)) goto storage_done;
        if (!granted) { result = 0; goto storage_done; }
    }
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    if (!uri_class || !intent_class) goto storage_done;
    jmethodID parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jmethodID construct = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jmethodID flags = (*env)->GetMethodID(env, intent_class, "addFlags", "(I)Landroid/content/Intent;");
    jmethodID start = (*env)->GetMethodID(env, context, "startActivity", "(Landroid/content/Intent;)V");
    jstring target = (*env)->NewStringUTF(env, "package:com.roblox.client");
    jstring action_text = (*env)->NewStringUTF(env, action);
    if (!parse || !construct || !flags || !start || !target || !action_text) goto storage_done;
    jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse, target);
    if (!uri || (*env)->ExceptionCheck(env)) goto storage_done;
    jobject intent = (*env)->NewObject(env, intent_class, construct, action_text, uri);
    if (!intent || (*env)->ExceptionCheck(env)) goto storage_done;
    (*env)->CallObjectMethod(env, intent, flags, 0x10000000);
    if ((*env)->ExceptionCheck(env)) goto storage_done;
    (*env)->CallVoidMethod(env, application, start, intent);
    if (!(*env)->ExceptionCheck(env)) result = 1;
storage_done:
    if (result < 0) java_error(env, error, error_size);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}

int device_open_discord_invite(char* error, size_t error_size)
{
    if (!java_vm) { snprintf(error, error_size, "Android runtime unavailable"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    { snprintf(error, error_size, "could not attach Discord invite thread"); return -1; }
    if (!env) { snprintf(error, error_size, "Android runtime unavailable"); return -1; }
    int result = -1;
    if ((*env)->PushLocalFrame(env, 20) < 0) goto finish;
    jclass activity = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!activity) goto done;
    jmethodID current = (*env)->GetStaticMethodID(env, activity, "currentApplication", "()Landroid/app/Application;");
    if (!current) goto done;
    jobject application = (*env)->CallStaticObjectMethod(env, activity, current);
    if (!application || (*env)->ExceptionCheck(env)) goto done;
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
    jclass context_class = (*env)->FindClass(env, "android/content/Context");
    if (!uri_class || !intent_class || !context_class) goto done;
    jmethodID parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jmethodID construct = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jmethodID flags = (*env)->GetMethodID(env, intent_class, "addFlags", "(I)Landroid/content/Intent;");
    jmethodID start = (*env)->GetMethodID(env, context_class, "startActivity", "(Landroid/content/Intent;)V");
    if (!parse || !construct || !flags || !start) goto done;
    jstring address = (*env)->NewStringUTF(env, "https://discord.gg/basalt");
    jstring action = (*env)->NewStringUTF(env, "android.intent.action.VIEW");
    if (!address || !action) goto done;
    jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse, address);
    if (!uri || (*env)->ExceptionCheck(env)) goto done;
    jobject intent = (*env)->NewObject(env, intent_class, construct, action, uri);
    if (!intent || (*env)->ExceptionCheck(env)) goto done;
    (*env)->CallObjectMethod(env, intent, flags, 0x10000000); // Application context requires NEW_TASK.
    if ((*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env, application, start, intent);
    if (!(*env)->ExceptionCheck(env)) result = 0;
done:
    if (result && !error[0]) java_error(env, error, error_size);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
finish:
    if (result && !error[0]) java_error(env, error, error_size);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}

int device_http_request(const char* request, size_t length, char** response,
                        size_t* response_length, char* error, size_t error_size)
{
    *response = NULL;
    *response_length = 0;
    if (!java_vm || !isolated_http_class || !isolated_http_execute || !request || !length ||
        length > 6u * MAX_HTTP_BODY + 65536u)
    { snprintf(error, error_size, "isolated HTTP runtime or request unavailable"); return -1; }
    JNIEnv* env = NULL;
    int attached = (*java_vm)->GetEnv(java_vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED;
    if (attached && (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    { snprintf(error, error_size, "could not attach HTTP worker"); return -1; }
    if (!env) { snprintf(error, error_size, "HTTP worker runtime unavailable"); return -1; }
    int result = -1;
    if ((*env)->PushLocalFrame(env, 16) < 0) goto finish;
    jbyteArray bytes = (*env)->NewByteArray(env, (jsize)length);
    if (!bytes) goto done;
    (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte*)request);
    if ((*env)->ExceptionCheck(env)) goto done;
    jbyteArray reply = (jbyteArray)(*env)->CallStaticObjectMethod(env, isolated_http_class, isolated_http_execute, bytes);
    if (!reply || (*env)->ExceptionCheck(env)) goto done;
    jsize size = (*env)->GetArrayLength(env, reply);
    if (size <= 0 || (size_t)size > 6u * MAX_HTTP_BODY + 65536u)
    { snprintf(error, error_size, "response envelope exceeds limit"); goto done; }
    char* output = malloc((size_t)size);
    if (!output) { snprintf(error, error_size, "HTTP allocation failed"); goto done; }
    (*env)->GetByteArrayRegion(env, reply, 0, size, (jbyte*)output);
    if ((*env)->ExceptionCheck(env)) { free(output); goto done; }
    *response = output;
    *response_length = (size_t)size;
    result = 0;
done:
    if (result && !error[0]) java_error(env, error, error_size);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
finish:
    if (result && !error[0]) java_error(env, error, error_size);
    if (attached) (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}

#if 0 /* Replaced by the isolated, credential-free transport above. */
int legacy_device_http_request(const char* request, size_t length, char** response,
                        size_t* response_length, char* error, size_t error_size)
{
    *response = NULL;
    *response_length = 0;
    if (!java_vm || !request || !length || length > 6u * MAX_HTTP_BODY + 65536u)
    {
        snprintf(error, error_size, "Android HTTP runtime or request unavailable");
        return -1;
    }

    JNIEnv* env = NULL;
    if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK)
    {
        snprintf(error, error_size, "could not attach Android HTTP worker");
        return -1;
    }

    int result = -1;
    jobject connection = NULL, stream = NULL, output_stream = NULL;
    jclass url_class = (*env)->FindClass(env, "java/net/URL");
    jclass connection_class = (*env)->FindClass(env, "java/net/HttpURLConnection");
    jclass urlconnection_class = (*env)->FindClass(env, "java/net/URLConnection");
    jclass object_class = (*env)->FindClass(env, "org/json/JSONObject");
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jclass input_class = (*env)->FindClass(env, "java/io/InputStream");
    jclass output_class = (*env)->FindClass(env, "java/io/OutputStream");
    jclass iterator_class = (*env)->FindClass(env, "java/util/Iterator");
    jclass base64_class = (*env)->FindClass(env, "android/util/Base64");
    if (!url_class || !connection_class || !urlconnection_class || !object_class ||
        !string_class || !input_class || !output_class || !iterator_class || !base64_class) goto done;

    jmethodID json_ctor = (*env)->GetMethodID(env, object_class, "<init>", "(Ljava/lang/String;)V");
    jmethodID json_empty = (*env)->GetMethodID(env, object_class, "<init>", "()V");
    jmethodID json_opt = (*env)->GetMethodID(env, object_class, "optString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID json_has = (*env)->GetMethodID(env, object_class, "has", "(Ljava/lang/String;)Z");
    jmethodID json_object = (*env)->GetMethodID(env, object_class, "optJSONObject", "(Ljava/lang/String;)Lorg/json/JSONObject;");
    jmethodID json_keys = (*env)->GetMethodID(env, object_class, "keys", "()Ljava/util/Iterator;");
    jmethodID json_put = (*env)->GetMethodID(env, object_class, "put", "(Ljava/lang/String;Ljava/lang/Object;)Lorg/json/JSONObject;");
    jmethodID json_int = (*env)->GetMethodID(env, object_class, "put", "(Ljava/lang/String;I)Lorg/json/JSONObject;");
    jmethodID json_bool = (*env)->GetMethodID(env, object_class, "put", "(Ljava/lang/String;Z)Lorg/json/JSONObject;");
    jmethodID json_string = (*env)->GetMethodID(env, object_class, "toString", "()Ljava/lang/String;");
    jmethodID iter_has = (*env)->GetMethodID(env, iterator_class, "hasNext", "()Z");
    jmethodID iter_next = (*env)->GetMethodID(env, iterator_class, "next", "()Ljava/lang/Object;");
    jmethodID url_ctor = (*env)->GetMethodID(env, url_class, "<init>", "(Ljava/lang/String;)V");
    jmethodID url_open = (*env)->GetMethodID(env, url_class, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID set_method = (*env)->GetMethodID(env, connection_class, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID set_follow = (*env)->GetMethodID(env, connection_class, "setInstanceFollowRedirects", "(Z)V");
    jmethodID get_code = (*env)->GetMethodID(env, connection_class, "getResponseCode", "()I");
    jmethodID get_reason = (*env)->GetMethodID(env, connection_class, "getResponseMessage", "()Ljava/lang/String;");
    jmethodID get_error = (*env)->GetMethodID(env, connection_class, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID disconnect = (*env)->GetMethodID(env, connection_class, "disconnect", "()V");
    jmethodID header_key = (*env)->GetMethodID(env, connection_class, "getHeaderFieldKey", "(I)Ljava/lang/String;");
    jmethodID header_value = (*env)->GetMethodID(env, connection_class, "getHeaderField", "(I)Ljava/lang/String;");
    jmethodID set_connect = (*env)->GetMethodID(env, urlconnection_class, "setConnectTimeout", "(I)V");
    jmethodID set_read = (*env)->GetMethodID(env, urlconnection_class, "setReadTimeout", "(I)V");
    jmethodID set_header = (*env)->GetMethodID(env, urlconnection_class, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID set_output = (*env)->GetMethodID(env, urlconnection_class, "setDoOutput", "(Z)V");
    jmethodID set_length = (*env)->GetMethodID(env, connection_class, "setFixedLengthStreamingMode", "(I)V");
    jmethodID get_output = (*env)->GetMethodID(env, urlconnection_class, "getOutputStream", "()Ljava/io/OutputStream;");
    jmethodID get_input = (*env)->GetMethodID(env, urlconnection_class, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID input_read = (*env)->GetMethodID(env, input_class, "read", "([B)I");
    jmethodID input_close = (*env)->GetMethodID(env, input_class, "close", "()V");
    jmethodID output_write = (*env)->GetMethodID(env, output_class, "write", "([B)V");
    jmethodID output_close = (*env)->GetMethodID(env, output_class, "close", "()V");
    jmethodID string_ctor = (*env)->GetMethodID(env, string_class, "<init>", "([BLjava/lang/String;)V");
    jmethodID string_bytes = (*env)->GetMethodID(env, string_class, "getBytes", "(Ljava/lang/String;)[B");
    jmethodID b64_decode = (*env)->GetStaticMethodID(env, base64_class, "decode", "(Ljava/lang/String;I)[B");
    jmethodID b64_encode = (*env)->GetStaticMethodID(env, base64_class, "encodeToString", "([BI)Ljava/lang/String;");
    if (!json_ctor || !json_empty || !json_opt || !json_has || !json_object || !json_keys ||
        !json_put || !json_int || !json_bool || !json_string || !iter_has || !iter_next ||
        !url_ctor || !url_open || !set_method || !set_follow || !get_code || !get_reason ||
        !get_error || !disconnect || !header_key || !header_value || !set_connect || !set_read ||
        !set_header || !set_output || !set_length || !get_output || !get_input || !input_read ||
        !input_close || !output_write || !output_close || !string_ctor || !string_bytes ||
        !b64_decode || !b64_encode) goto done;

    jbyteArray request_bytes = (*env)->NewByteArray(env, (jsize)length);
    jstring request_encoding = (*env)->NewStringUTF(env, "UTF-8");
    if (!request_bytes || !request_encoding) goto done;
    (*env)->SetByteArrayRegion(env, request_bytes, 0, (jsize)length, (const jbyte*)request);
    if ((*env)->ExceptionCheck(env)) goto done;
    jstring input = (jstring)(*env)->NewObject(env, string_class, string_ctor, request_bytes, request_encoding);
    if (!input) goto done;
    jobject options = (*env)->NewObject(env, object_class, json_ctor, input);
    if (!options || (*env)->ExceptionCheck(env)) goto done;

    jstring url_name = (*env)->NewStringUTF(env, "Url");
    jstring url_alias = (*env)->NewStringUTF(env, "URL");
    jstring method_name = (*env)->NewStringUTF(env, "Method");
    jstring default_method = (*env)->NewStringUTF(env, "GET");
    jstring headers_name = (*env)->NewStringUTF(env, "Headers");
    jstring body_name = (*env)->NewStringUTF(env, "Body");
    jstring b64_name = (*env)->NewStringUTF(env, "BodyBase64");
    jstring empty = (*env)->NewStringUTF(env, "");
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    if (!url_name || !url_alias || !method_name || !default_method || !headers_name ||
        !body_name || !b64_name || !empty || !utf8) goto done;

    jstring url_string = (jstring)(*env)->CallObjectMethod(env, options, json_opt, url_name, empty);
    if (!url_string || (*env)->ExceptionCheck(env)) goto done;
    if (!(*env)->GetStringLength(env, url_string))
        url_string = (jstring)(*env)->CallObjectMethod(env, options, json_opt, url_alias, empty);
    jstring method_string = (jstring)(*env)->CallObjectMethod(env, options, json_opt, method_name, default_method);
    if (!url_string || !method_string || (*env)->ExceptionCheck(env)) goto done;
    const char* url_text = (*env)->GetStringUTFChars(env, url_string, NULL);
    const char* method_text = (*env)->GetStringUTFChars(env, method_string, NULL);
    if (!url_text || !method_text) goto done;
    int url_ok = !strncmp(url_text, "https://", 8) || !strncmp(url_text, "http://", 7);
    int method_ok = valid_method(method_text);
    (*env)->ReleaseStringUTFChars(env, url_string, url_text);
    (*env)->ReleaseStringUTFChars(env, method_string, method_text);
    if (!url_ok || !method_ok)
    {
        snprintf(error, error_size, "Url must be http(s), and Method must be GET, POST, PUT, PATCH, DELETE or HEAD");
        goto done;
    }

    jobject url = (*env)->NewObject(env, url_class, url_ctor, url_string);
    if (!url || (*env)->ExceptionCheck(env)) goto done;
    connection = (*env)->CallObjectMethod(env, url, url_open);
    if (!connection || (*env)->ExceptionCheck(env)) goto done;
    (*env)->CallVoidMethod(env, connection, set_connect, 15000);
    (*env)->CallVoidMethod(env, connection, set_read, 15000);
    (*env)->CallVoidMethod(env, connection, set_follow, JNI_TRUE);
    (*env)->CallVoidMethod(env, connection, set_method, method_string);
    if ((*env)->ExceptionCheck(env)) goto done;

    jobject headers = (*env)->CallObjectMethod(env, options, json_object, headers_name);
    int has_user_agent = 0;
    if (headers)
    {
        jobject keys = (*env)->CallObjectMethod(env, headers, json_keys);
        if (!keys || (*env)->ExceptionCheck(env)) goto done;
        int count = 0;
        while ((*env)->CallBooleanMethod(env, keys, iter_has))
        {
            if ((*env)->ExceptionCheck(env)) goto done;
            if (++count > 80) { snprintf(error, error_size, "too many HTTP headers"); goto done; }
            jstring name = (jstring)(*env)->CallObjectMethod(env, keys, iter_next);
            jstring value = (jstring)(*env)->CallObjectMethod(env, headers, json_opt, name, empty);
            if (!name || !value || (*env)->ExceptionCheck(env)) goto done;
            const char* n = (*env)->GetStringUTFChars(env, name, NULL);
            const char* v = (*env)->GetStringUTFChars(env, value, NULL);
            if (!n || !v) goto done;
            int invalid = !n[0] || strchr(n, '\r') || strchr(n, '\n') || strchr(v, '\r') || strchr(v, '\n');
            if (!strcasecmp(n, "User-Agent")) has_user_agent = 1;
            (*env)->ReleaseStringUTFChars(env, name, n);
            (*env)->ReleaseStringUTFChars(env, value, v);
            if (invalid) { snprintf(error, error_size, "invalid HTTP header"); goto done; }
            (*env)->CallVoidMethod(env, connection, set_header, name, value);
            (*env)->DeleteLocalRef(env, name);
            (*env)->DeleteLocalRef(env, value);
            if ((*env)->ExceptionCheck(env)) goto done;
        }
        if ((*env)->ExceptionCheck(env)) goto done;
    }
    if (!has_user_agent)
    {
        jstring ua = (*env)->NewStringUTF(env, "User-Agent");
        jstring client = (*env)->NewStringUTF(env, "Roblox/WinInet");
        if (!ua || !client) goto done;
        (*env)->CallVoidMethod(env, connection, set_header, ua, client);
        if ((*env)->ExceptionCheck(env)) goto done;
    }

    int encoded_body = (*env)->CallBooleanMethod(env, options, json_has, b64_name);
    int plain_body = (*env)->CallBooleanMethod(env, options, json_has, body_name);
    if ((*env)->ExceptionCheck(env)) goto done;
    if (encoded_body && plain_body) { snprintf(error, error_size, "Body and BodyBase64 cannot both be set"); goto done; }
    jbyteArray payload = NULL;
    if (encoded_body)
    {
        jstring encoded = (jstring)(*env)->CallObjectMethod(env, options, json_opt, b64_name, empty);
        if (!encoded || (*env)->ExceptionCheck(env)) goto done;
        payload = (jbyteArray)(*env)->CallStaticObjectMethod(env, base64_class, b64_decode, encoded, 2);
    }
    else if (plain_body)
    {
        jstring plain = (jstring)(*env)->CallObjectMethod(env, options, json_opt, body_name, empty);
        if (!plain || (*env)->ExceptionCheck(env)) goto done;
        payload = (jbyteArray)(*env)->CallObjectMethod(env, plain, string_bytes, utf8);
    }
    if ((*env)->ExceptionCheck(env)) goto done;
    if (payload)
    {
        jint bytes = (*env)->GetArrayLength(env, payload);
        if (bytes > (jint)MAX_HTTP_BODY) { snprintf(error, error_size, "Body exceeds 8 MiB"); goto done; }
        (*env)->CallVoidMethod(env, connection, set_output, JNI_TRUE);
        (*env)->CallVoidMethod(env, connection, set_length, bytes);
        output_stream = (*env)->CallObjectMethod(env, connection, get_output);
        if (!output_stream || (*env)->ExceptionCheck(env)) goto done;
        (*env)->CallVoidMethod(env, output_stream, output_write, payload);
        (*env)->CallVoidMethod(env, output_stream, output_close);
        output_stream = NULL;
        if ((*env)->ExceptionCheck(env)) goto done;
    }

    jint status = (*env)->CallIntMethod(env, connection, get_code);
    jstring reason = (jstring)(*env)->CallObjectMethod(env, connection, get_reason);
    if ((*env)->ExceptionCheck(env)) goto done;
    if (!reason) reason = empty;
    stream = status >= 400 ? (*env)->CallObjectMethod(env, connection, get_error) :
                             (*env)->CallObjectMethod(env, connection, get_input);
    if ((*env)->ExceptionCheck(env)) goto done;

    uint8_t* body = NULL;
    size_t used = 0, capacity = 0;
    if (stream)
    {
        jbyteArray chunk = (*env)->NewByteArray(env, READ_CHUNK);
        if (!chunk) goto done;
        for (;;)
        {
            jint count = (*env)->CallIntMethod(env, stream, input_read, chunk);
            if ((*env)->ExceptionCheck(env)) { free(body); goto done; }
            if (count == -1) break;
            if (count == 0) continue;
            if (count > (jint)MAX_HTTP_BODY || used > MAX_HTTP_BODY - (size_t)count)
            { free(body); snprintf(error, error_size, "response exceeds 8 MiB"); goto done; }
            if (used + (size_t)count > capacity)
            {
                size_t next = capacity ? capacity * 2 : READ_CHUNK;
                while (next < used + (size_t)count) next *= 2;
                if (next > MAX_HTTP_BODY) next = MAX_HTTP_BODY;
                uint8_t* more = realloc(body, next);
                if (!more) { free(body); snprintf(error, error_size, "out of memory"); goto done; }
                body = more;
                capacity = next;
            }
            (*env)->GetByteArrayRegion(env, chunk, 0, count, (jbyte*)body + used);
            if ((*env)->ExceptionCheck(env)) { free(body); goto done; }
            used += (size_t)count;
        }
        (*env)->CallVoidMethod(env, stream, input_close);
        stream = NULL;
        if ((*env)->ExceptionCheck(env)) { free(body); goto done; }
    }

    jobject reply = (*env)->NewObject(env, object_class, json_empty);
    jobject reply_headers = (*env)->NewObject(env, object_class, json_empty);
    if (!reply || !reply_headers || (*env)->ExceptionCheck(env)) { free(body); goto done; }
    for (int i = 0; i < 100; ++i)
    {
        jstring key = (jstring)(*env)->CallObjectMethod(env, connection, header_key, i);
        jstring value = (jstring)(*env)->CallObjectMethod(env, connection, header_value, i);
        if ((*env)->ExceptionCheck(env)) { free(body); goto done; }
        if (!key && !value) break;
        if (key && value) (*env)->CallObjectMethod(env, reply_headers, json_put, key, value);
        if (key) (*env)->DeleteLocalRef(env, key);
        if (value) (*env)->DeleteLocalRef(env, value);
        if ((*env)->ExceptionCheck(env)) { free(body); goto done; }
    }
    jstring success_name = (*env)->NewStringUTF(env, "Success");
    jstring code_name = (*env)->NewStringUTF(env, "StatusCode");
    jstring reason_name = (*env)->NewStringUTF(env, "StatusMessage");
    jstring reply_headers_name = (*env)->NewStringUTF(env, "Headers");
    if (!success_name || !code_name || !reason_name || !reply_headers_name)
    { free(body); goto done; }
    (*env)->CallObjectMethod(env, reply, json_bool, success_name, status >= 200 && status < 300);
    (*env)->CallObjectMethod(env, reply, json_int, code_name, status);
    (*env)->CallObjectMethod(env, reply, json_put, reason_name, reason);
    (*env)->CallObjectMethod(env, reply, json_put, reply_headers_name, reply_headers);
    if ((*env)->ExceptionCheck(env)) { free(body); goto done; }

    jbyteArray content = (*env)->NewByteArray(env, (jsize)used);
    if (!content) { free(body); goto done; }
    if (used) (*env)->SetByteArrayRegion(env, content, 0, (jsize)used, (const jbyte*)body);
    if ((*env)->ExceptionCheck(env)) { free(body); goto done; }
    if (valid_utf8(body, used))
    {
        jstring text = (jstring)(*env)->NewObject(env, string_class, string_ctor, content, utf8);
        if (text) (*env)->CallObjectMethod(env, reply, json_put, body_name, text);
    }
    else
    {
        jstring encoded = (jstring)(*env)->CallStaticObjectMethod(env, base64_class, b64_encode, content, 2);
        if (encoded) (*env)->CallObjectMethod(env, reply, json_put, b64_name, encoded);
    }
    free(body);
    if ((*env)->ExceptionCheck(env)) goto done;

    jstring serialized = (jstring)(*env)->CallObjectMethod(env, reply, json_string);
    if (!serialized || (*env)->ExceptionCheck(env)) goto done;
    jbyteArray bytes = (jbyteArray)(*env)->CallObjectMethod(env, serialized, string_bytes, utf8);
    if (!bytes || (*env)->ExceptionCheck(env)) goto done;
    jsize size = (*env)->GetArrayLength(env, bytes);
    if (size <= 0 || (size_t)size > 6u * MAX_HTTP_BODY + 65536u)
    { snprintf(error, error_size, "response envelope exceeds limit"); goto done; }
    char* buffer = malloc(size);
    if (!buffer) { snprintf(error, error_size, "out of memory"); goto done; }
    (*env)->GetByteArrayRegion(env, bytes, 0, size, (jbyte*)buffer);
    if ((*env)->ExceptionCheck(env)) { free(buffer); goto done; }
    *response = buffer;
    *response_length = (size_t)size;
    result = 0;

done:
    if (result && !error[0]) java_error(env, error, error_size);
    else if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (stream)
    {
        (*env)->CallVoidMethod(env, stream, input_close);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    if (output_stream)
    {
        (*env)->CallVoidMethod(env, output_stream, output_close);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    if (connection)
    {
        jmethodID close_connection = (*env)->GetMethodID(env, connection_class, "disconnect", "()V");
        if (close_connection) (*env)->CallVoidMethod(env, connection, close_connection);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    (*java_vm)->DetachCurrentThread(java_vm);
    return result;
}
#endif
