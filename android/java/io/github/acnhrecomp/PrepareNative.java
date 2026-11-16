// The game folder's preparation on the phone: libacnh_prepare.so (prepare/prepare_jni.cpp, the stages of
// prepare/prepare.h), apart from the player's libacnh.so. Text in and out as UTF-8: a request is "key=value" lines
// (input=, dlc=, out=, in_place=1, platforms=, fps60=, zig=, zig_lib=, shader_tool=, cache=, log=), the answers JSON.
package io.github.acnhrecomp;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;

final class PrepareNative {
    static {
        System.loadLibrary("acnh_prepare");
    }

    private PrepareNative() {}

    private static native byte[] nativeProbe(byte[] request);
    private static native boolean nativeStart(byte[] request);
    private static native byte[] nativeStatus(long since);
    private static native void nativeCancel();

    private static byte[] utf8(String s) { return s.getBytes(StandardCharsets.UTF_8); }

    private static JSONObject json(byte[] b) {
        try {
            return new JSONObject(new String(b, StandardCharsets.UTF_8));
        } catch (Exception e) {
            return new JSONObject();
        }
    }

    // The folders' check: ok, version, romfs_files, romfs_bytes, dlc[], messages[] (severity, key, args, text).
    static JSONObject probe(String request) { return json(nativeProbe(utf8(request))); }

    // false when a preparation is running already (or the platforms are wrong).
    static boolean start(String request) { return nativeStart(utf8(request)); }

    // stage, stage_n, fraction, overall, item, running, finished, rc, error, out, messages[], log[] (n, stage, text:
    // the lines numbered `since` and after), log_next.
    static JSONObject status(long since) { return json(nativeStatus(since)); }

    // Stops the running preparation soon (a child process too), keeping what is complete.
    static void cancel() { nativeCancel(); }
}
