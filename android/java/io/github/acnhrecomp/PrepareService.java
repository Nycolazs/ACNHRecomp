// The game folder's preparation on the phone, as a foreground service (type specialUse): it takes an hour or more,
// and a plain background app would be stopped long before. Holds a partial wake lock, shows its progress in a
// notification (with Cancel), gets the tools ready (zig's lib folder unpacked from the APK once per app version) and
// runs PrepareNative; the Prepare screen polls the same native status. Done, it saves the game folder in
// <files>/data_root.txt, so the app opens into the game (Home.java).
//
// The tools are the APK's native libraries, the only files Android lets an app run (tools/build_android.sh):
// libzig.so (zig for aarch64-linux) and libspirv_harness.so (the shader tool, NativeAOT for linux-bionic-arm64), in
// the app's native library folder; zig's lib folder, assets/zig-lib.zip with its sizes.txt, goes to <files>/zig-lib
// and zig's cache to <cache>/zig-cache (the app's own storage, off the FUSE of the shared one), removed once done.
package io.github.acnhrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import static io.github.acnhrecomp.Screen.t;

public class PrepareService extends Service {
    static final int IDLE = 0, TOOLS = 1, RUNNING = 2, FINISHED = 3;
    private static final String CHANNEL = "prepare", ACTION_CANCEL = "io.github.acnhrecomp.PREPARE_CANCEL";
    private static final int NOTE_RUNNING = 1, NOTE_DONE = 2;

    // What the Prepare screen shows: the phase, when it began, and once FINISHED how it ended: the last native status
    // (rc 0 done, 1 a stage failed, 2 refused, 4 cancelled; error, out), or rc 1 with "translated" when the tools
    // could not be made ready, rc 4 when cancelled before the preparation itself began.
    static volatile int phase = IDLE;
    static volatile long started;
    static volatile JSONObject result;
    private static volatile boolean cancelled;
    private static Thread worker;

    private PowerManager.WakeLock wake;

    static boolean active() { return phase == TOOLS || phase == RUNNING; }

    // Starts the preparation of `request` (PrepareNative's request: the inputs, the game folder, ...): the phase is
    // TOOLS from here, so the screen shows it running at once.
    static void begin(Context c, String request) {
        phase = TOOLS;
        started = System.currentTimeMillis();
        result = null;
        cancelled = false;
        try {
            c.startForegroundService(new Intent(c, PrepareService.class).putExtra("request", request));
        } catch (Exception e) {  // Android refusing a foreground service now (the app not in front)
            result = ended(1, t("The preparation could not start: ", "A preparação não conseguiu começar: ") + e.getMessage());
            phase = FINISHED;
        }
    }

    private static JSONObject ended(int rc, String why) {
        JSONObject r = new JSONObject();
        try {
            r.put("finished", true).put("rc", rc).put("error", why == null ? "" : why).put("translated", true);
        } catch (Exception e) {
            // put() with these values does not throw
        }
        return r;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public int onStartCommand(Intent intent, int flags, int id) {
        if (intent != null && ACTION_CANCEL.equals(intent.getAction())) {
            cancel();
            if (!active()) stopSelf();
            return START_NOT_STICKY;
        }
        // Android asks for startForeground() soon after startForegroundService(), also when nothing is started
        foreground(running(t("Getting ready", "Preparando"), 0));
        final String request = intent == null ? null : intent.getStringExtra("request");
        if (worker != null && worker.isAlive()) return START_NOT_STICKY;  // one at a time
        if (request == null || phase != TOOLS) {
            stop();
            return START_NOT_STICKY;
        }
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "acnhrecomp:prepare");
        wake.setReferenceCounted(false);
        wake.acquire();
        worker = new Thread(() -> run(request), "prepare");
        worker.start();
        return START_NOT_STICKY;
    }

    static void cancel() {
        cancelled = true;
        if (phase == RUNNING) PrepareNative.cancel();
    }

    private void run(String request) {
        String tools;
        try {
            tools = tools();
        } catch (Exception e) {
            done(null, t("The tools that prepare the game could not be made ready: ",
                         "As ferramentas que preparam o jogo não ficaram prontas: ") + e.getMessage());
            return;
        }
        if (cancelled) {
            done(null, null);
            return;
        }
        if (!PrepareNative.start(request + tools)) {
            done(null, t("The preparation could not start.", "A preparação não conseguiu começar."));
            return;
        }
        phase = RUNNING;
        if (cancelled) PrepareNative.cancel();
        JSONObject st;
        for (;;) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                // polled again
            }
            st = PrepareNative.status(Long.MAX_VALUE);  // no log lines: the screen takes those
            if (st.optBoolean("finished")) break;
            int pct = (int) Math.round(st.optDouble("overall") * 100);
            getSystemService(NotificationManager.class).notify(NOTE_RUNNING,
                    running(Prepare.stageText(st.optString("stage")) + " · " + pct + "%", st.optDouble("overall")));
        }
        done(st, null);
    }

    // The tools' request lines: the APK's native libraries, zig's lib folder (unpacked when this app version has not
    // yet), its cache, the log.
    private String tools() throws Exception {
        File dir = new File(getApplicationInfo().nativeLibraryDir);
        File zig = new File(dir, "libzig.so"), shaders = new File(dir, "libspirv_harness.so");
        if (!zig.isFile() || !shaders.isFile())
            throw new Exception(t("this build of the app does not carry them (" + dir + ")",
                                  "esta versão do app não as traz (" + dir + ")"));
        File lib = new File(getFilesDir(), "zig-lib"), mark = new File(getFilesDir(), "zig-lib.version");
        String version = String.valueOf(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        if (!lib.isDirectory() || !version.equals(read(mark))) {
            getSystemService(NotificationManager.class).notify(NOTE_RUNNING,
                    running(t("Unpacking the compiler (once)", "Descompactando o compilador (uma vez)"), 0));
            mark.delete();
            File tmp = new File(getFilesDir(), "zig-lib.tmp");
            delete(tmp);
            delete(lib);
            unzip("zig-lib.zip", tmp);
            if (!tmp.renameTo(lib)) throw new Exception("cannot rename " + tmp + " to " + lib);
            try (OutputStream o = new FileOutputStream(mark)) {
                o.write(version.getBytes(StandardCharsets.UTF_8));
            }
        }
        File cache = new File(getCacheDir(), "zig-cache");
        cache.mkdirs();
        // the log where a computer reaches it over USB (Android/data/<app>/files), else the app's own files
        File logs = getExternalFilesDir(null);
        if (logs == null) logs = getFilesDir();
        return "zig=" + zig + "\nzig_lib=" + lib + "\nshader_tool=" + shaders + "\ncache=" + cache + "\nlog="
                + new File(logs, "prepare.log") + "\n";
    }

    private void unzip(String asset, File to) throws Exception {
        String root = to.getCanonicalPath() + File.separator;
        byte[] buf = new byte[1 << 16];
        try (InputStream in = getAssets().open(asset); ZipInputStream z = new ZipInputStream(in)) {
            for (ZipEntry e; (e = z.getNextEntry()) != null;) {
                File f = new File(to, e.getName());
                if (!f.getCanonicalPath().startsWith(root)) continue;  // no entry outside the folder
                if (e.isDirectory()) {
                    f.mkdirs();
                    continue;
                }
                f.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(f)) {
                    for (int n; (n = z.read(buf)) > 0;) o.write(buf, 0, n);
                }
                if (cancelled) throw new Exception(t("cancelled", "cancelado"));
            }
        }
    }

    static String read(File f) {
        try {
            return new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    static void delete(File f) {
        File[] in = f.isDirectory() ? f.listFiles() : null;
        if (in != null)
            for (File c : in) delete(c);
        f.delete();
    }

    // The end: the game folder saved for Home when it is ready, the notification of how it went, the service gone.
    private void done(JSONObject st, String why) {
        String title, text;
        if (st != null && st.optInt("rc", -1) == 0) {
            String folder = st.optString("out");
            try (OutputStream o = new FileOutputStream(new File(getFilesDir(), "data_root.txt"))) {
                o.write((folder + "\n").getBytes(StandardCharsets.UTF_8));
            } catch (Exception e) {
                // Prepare's Play button hands the folder to Home as well
            }
            delete(new File(getCacheDir(), "zig-cache"));
            title = t("The game is ready", "O jogo está pronto");
            text = t("Tap to play", "Toque para jogar");
        } else if (cancelled || (st != null && st.optInt("rc") == 4)) {
            title = t("Preparation cancelled", "Preparação cancelada");
            text = t("What was done is kept: start again to go on.", "O que foi feito fica: comece de novo para continuar.");
        } else {
            title = t("The preparation stopped", "A preparação parou");
            text = why != null ? why : st != null ? st.optString("error") : "";
        }
        result = st != null ? st : ended(cancelled ? 4 : 1, why);
        phase = FINISHED;
        NotificationManager nm = getSystemService(NotificationManager.class);
        Notification.Builder b = builder().setContentTitle(title).setContentText(text).setAutoCancel(true)
                .setStyle(new Notification.BigTextStyle().bigText(text));
        nm.notify(NOTE_DONE, b.build());
        stop();
    }

    private void stop() {
        if (wake != null && wake.isHeld()) wake.release();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private Notification.Builder builder() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm.getNotificationChannel(CHANNEL) == null)
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, t("Preparing the game", "Preparação do jogo"),
                    NotificationManager.IMPORTANCE_LOW));
        Intent open = new Intent(this, Prepare.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentIntent(PendingIntent.getActivity(this, 0, open,
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
    }

    private Notification running(String text, double overall) {
        Intent stop = new Intent(this, PrepareService.class).setAction(ACTION_CANCEL);
        PendingIntent cancel = PendingIntent.getService(this, 1, stop, PendingIntent.FLAG_IMMUTABLE);
        return builder()
                .setContentTitle(t("Preparing the game", "Preparando o jogo"))
                .setContentText(text)
                .setProgress(1000, (int) Math.round(overall * 1000), false)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .addAction(new Notification.Action.Builder(
                        Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel),
                        t("Cancel", "Cancelar"), cancel).build())
                .build();
    }

    private void foreground(Notification n) {
        if (Build.VERSION.SDK_INT >= 34) startForeground(NOTE_RUNNING, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else startForeground(NOTE_RUNNING, n);
    }
}
