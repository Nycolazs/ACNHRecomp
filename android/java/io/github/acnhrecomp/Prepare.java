// The game folder prepared on the phone (prepare/README.md), from the folder Home was given: the ExeFS and RomFS the
// player extracted from their own copy of the game with their emulator (never keys, NSP or XCI). Shows the check of
// those files (PrepareNative.probe, its findings translated), the game folder it will write (by default "ACNH Recomp
// Game" beside the extracted one, on shared storage: the app's own folders go when it is uninstalled), an optional
// add-on folder and Start; then the stages, the progress and Cancel while PrepareService runs it, and Play once done.
//
// Intent extras: "source" the folder picked on Home; "out" a game folder to prepare again where it is (one prepared
// for another version of the app). Opened with neither (the notification), it shows the preparation as it stands.
package io.github.acnhrecomp;

import android.app.Activity;
import android.app.NotificationManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static io.github.acnhrecomp.Screen.t;

public class Prepare extends Activity {
    private static final int PICK_OUT = 1, PICK_DLC = 2, ASK_NOTIFY = 3;
    private static final String GAME_NAME = "ACNH Recomp Game";
    private static final String[] STAGES = {"check", "exefs", "recompile", "shaders", "packs", "compile", "finish"};

    private SharedPreferences prefs;
    private String source, out, dlc = "";
    private boolean inPlace, fps60 = true, again;
    private JSONObject probe;  // the last check of the files (null while it runs)
    private long logNext;
    private final List<String> log = new ArrayList<>();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean polling, cancelAsked;
    private LinearLayout box;
    private TextView stagesView, itemView, timeView, logView, percentView;
    private ProgressBar bar;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("prepare", MODE_PRIVATE);
        route(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        route(intent);
    }

    private void route(Intent i) {
        String src = i.getStringExtra("source");
        if (PrepareService.active()) {
            showRunning();
        } else if (src != null) {
            if (PrepareService.phase == PrepareService.FINISHED) PrepareService.phase = PrepareService.IDLE;
            setSource(src, i.getStringExtra("out"));
            showForm();
            check();
        } else if (PrepareService.phase == PrepareService.FINISHED && PrepareService.result != null) {
            showFinished(PrepareService.result);
        } else {
            home();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (stagesView != null) startPolling();  // the running screen: on, or its end shown
    }

    @Override
    protected void onPause() {
        super.onPause();
        polling = false;
        handler.removeCallbacksAndMessages(null);
    }

    // Running, Back leaves the app (the preparation goes on, its notification brings this back); its end, once seen,
    // is not shown again.
    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (PrepareService.active()) {
            moveTaskToBack(true);
            return;
        }
        if (PrepareService.phase == PrepareService.FINISHED) PrepareService.phase = PrepareService.IDLE;
        super.onBackPressed();
    }

    private void home() {
        Intent h = new Intent(this, Home.class).putExtra("pick", true).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP);
        startActivity(h);
        finish();
    }

    // ---- the form -----------------------------------------------------------------------------------------------

    private void setSource(String src, String outAgain) {
        source = src;
        again = outAgain != null;
        boolean same = src.equals(prefs.getString("source", null));
        dlc = same ? prefs.getString("dlc", "") : "";
        fps60 = prefs.getBoolean("fps60", true);
        inPlace = false;
        if (again) {
            out = outAgain;
        } else {
            out = same ? prefs.getString("out", null) : null;
            inPlace = same && prefs.getBoolean("in_place", false);
            if (out == null || out.isEmpty()) out = defaultOut(new File(src));
        }
    }

    // "ACNH Recomp Game" beside the extracted folder (inside it when its parent cannot be written: the storage's top),
    // or "ACNH Recomp Game 2", ... when one of that name holds other things.
    static String defaultOut(File src) {
        File parent = src.getParentFile();
        return freeName(parent != null && parent.canWrite() ? parent : src);
    }

    private static String freeName(File base) {
        for (int k = 1; k < 100; k++) {
            File f = new File(base, k == 1 ? GAME_NAME : GAME_NAME + " " + k);
            if (usable(f)) return f.getPath();
        }
        return new File(base, GAME_NAME).getPath();
    }

    // A folder the preparation takes as the game folder: none yet, empty, or prepared before.
    private static boolean usable(File f) {
        if (!f.exists()) return true;
        String[] in = f.list();
        return in != null && (in.length == 0 || prepared(f));
    }

    private static boolean prepared(File f) {
        return new File(f, "game.ini").exists() || new File(f, ".import").exists() || new File(f, "exefs").exists();
    }

    private void check() {
        probe = null;
        final String req = "input=" + source + "\n" + (dlc.isEmpty() ? "" : "dlc=" + dlc + "\n");
        new Thread(() -> {
            final JSONObject p = PrepareNative.probe(req);
            runOnUiThread(() -> {
                probe = p;
                if (!PrepareService.active() && PrepareService.phase != PrepareService.FINISHED) showForm();
            });
        }, "probe").start();
    }

    private void showForm() {
        polling = false;
        stagesView = null;
        box = Screen.page(this, again ? t("Prepare the game again", "Preparar o jogo de novo")
                                      : t("Prepare the game", "Preparar o jogo"));
        box.addView(Screen.text(this, again
                ? t("This game folder was prepared for another version of the app. The phone prepares it again where "
                    + "it is, from its own files: what is still right is kept.",
                    "Esta pasta do jogo foi preparada para outra versão do app. O celular a prepara de novo onde ela "
                    + "está, a partir dos arquivos dela: o que ainda está certo fica.")
                : t("The phone prepares the game folder from the ExeFS and RomFS you extracted from your own copy of "
                    + "the game (update 3.0.3) with your emulator. Nothing is downloaded, and no keys, NSP or XCI "
                    + "are used.",
                    "O celular prepara a pasta do jogo a partir do ExeFS e do RomFS que você extraiu da sua cópia do "
                    + "jogo (atualização 3.0.3) com o seu emulador. Nada é baixado, e não são usadas chaves, NSP "
                    + "nem XCI."), 16, Screen.TEXT));
        TextView note = Screen.text(this, t("It takes long on a phone: often an hour or more, most of it compiling "
                        + "the game's code, and the phone gets warm. Keep it charging; you can leave the app, a "
                        + "notification shows how it goes. A computer does it much faster: ACNHRecomp's launcher "
                        + "there can prepare the folder for Android (platform android-arm64); copy that folder to "
                        + "the phone and choose it on the first screen.",
                "No celular demora: muitas vezes uma hora ou mais, a maior parte compilando o código do jogo, e o "
                        + "celular esquenta. Deixe-o carregando; você pode sair do app, uma notificação mostra o "
                        + "andamento. Um computador faz isso bem mais rápido: o launcher do ACNHRecomp nele pode "
                        + "preparar a pasta para Android (plataforma android-arm64); copie essa pasta para o "
                        + "celular e escolha-a na primeira tela."), 14, Screen.ORANGE);
        box.addView(note);

        box.addView(Screen.heading(this, t("Your files", "Os seus arquivos")));
        box.addView(Screen.text(this, source, 14, Screen.TEXT));
        if (probe == null) {
            box.addView(Screen.text(this, t("Checking the files (on a phone this can take a minute)...",
                    "Verificando os arquivos (no celular pode levar um minuto)..."), 15, Screen.TEXT));
            box.addView(new ProgressBar(this));
        } else {
            addMessages(probe.optJSONArray("messages"), false);
        }

        if (!again) {
            box.addView(Screen.heading(this, t("Add-on (optional)", "Conteúdo adicional (opcional)")));
            box.addView(Screen.text(this, dlc.isEmpty()
                    ? t("Happy Home Paradise: the RomFS of the add-on, extracted like the game's. An aoc folder beside "
                        + "exefs and romfs is found on its own.",
                        "Happy Home Paradise: o RomFS do conteúdo adicional, extraído como o do jogo. Uma pasta aoc "
                        + "ao lado de exefs e romfs é encontrada sozinha.")
                    : dlc, 14, Screen.TEXT));
            box.addView(Screen.button(this, dlc.isEmpty() ? t("Choose the add-on's folder", "Escolher a pasta do conteúdo adicional")
                                                          : t("No add-on", "Sem conteúdo adicional"), Screen.WHITE, v -> {
                if (dlc.isEmpty()) {
                    pick(PICK_DLC);
                } else {
                    dlc = "";
                    showForm();
                    check();
                }
            }));

            box.addView(Screen.heading(this, t("The game folder", "A pasta do jogo")));
            boolean canInPlace = probe != null && inPlacePossible();
            if (!canInPlace) inPlace = false;
            box.addView(Screen.text(this, inPlace ? folderInPlace() + "\n" + t("(the extracted folder itself)", "(a própria pasta extraída)") : out,
                    14, Screen.TEXT));
            if (!inPlace)
                box.addView(Screen.button(this, t("Choose another folder", "Escolher outra pasta"), Screen.WHITE, v -> pick(PICK_OUT)));
            if (canInPlace) {
                CheckBox here = checkBox(t("Use the extracted folder as the game folder: no copy of the RomFS (about "
                                           + gb(probe.optLong("romfs_bytes")) + " less)",
                                           "Usar a pasta extraída como pasta do jogo: sem cópia do RomFS (cerca de "
                                           + gb(probe.optLong("romfs_bytes")) + " a menos)"), inPlace);
                here.setOnCheckedChangeListener((b, on) -> {
                    inPlace = on;
                    showForm();
                });
                box.addView(here);
            }
        }
        CheckBox fps = checkBox(t("Native 60 FPS (tmouh's SpeedFix)", "60 FPS nativo (SpeedFix do tmouh)"), fps60);
        fps.setOnCheckedChangeListener((b, on) -> fps60 = on);
        box.addView(fps);

        boolean ok = probe != null && probe.optBoolean("ok");
        if (probe != null) {
            long need = need(), free = freeSpace(inPlace ? folderInPlace() : out);
            TextView space = Screen.text(this, t("Space: about " + gb(need) + " needed, " + gb(free) + " free there.",
                    "Espaço: cerca de " + gb(need) + " necessários, " + gb(free) + " livres lá."), 15,
                    free >= 0 && free < need ? Screen.RED : Screen.TEXT);
            box.addView(space);
        }
        if (probe != null && !ok)
            box.addView(Screen.text(this, t("Fix what is marked above, then choose the folder again.",
                    "Corrija o que está marcado acima e escolha a pasta de novo."), 15, Screen.RED));
        android.widget.Button start = Screen.button(this, t("Start", "Começar"), Screen.GREEN, v -> askAndStart());
        start.setEnabled(ok);
        start.setAlpha(ok ? 1f : 0.45f);
        box.addView(start);
        box.addView(Screen.button(this, t("Choose another folder to play", "Escolher outra pasta para jogar"), Screen.BLUE, v -> home()));
    }

    private CheckBox checkBox(String text, boolean on) {
        CheckBox c = new CheckBox(this);
        c.setText(text);
        c.setTextSize(15);
        c.setTextColor(Screen.BROWN);
        c.setChecked(on);
        c.setPadding(0, Screen.dp(this, 8), 0, Screen.dp(this, 8));
        return c;
    }

    // In place: the RomFS is <source>/romfs (or <source>/extract/romfs), so the game folder is the extracted one.
    private boolean inPlacePossible() {
        File r = new File(probe.optString("romfs"));
        if (!r.getName().equalsIgnoreCase("romfs") || r.getParentFile() == null) return false;
        File p = r.getParentFile();
        if (p.getName().equalsIgnoreCase("extract") && p.getParentFile() != null) p = p.getParentFile();
        return p.getAbsolutePath().equals(new File(source).getAbsolutePath());
    }

    private String folderInPlace() {
        File p = new File(probe.optString("romfs")).getParentFile();
        if (p != null && p.getName().equalsIgnoreCase("extract")) p = p.getParentFile();
        return p == null ? source : p.getPath();
    }

    // As prepare.cpp's check counts it: the RomFS's copy (unless in place), the add-ons, the shaders, the recompiled
    // code with one platform's objects, the logs.
    private long need() {
        long n = 300L << 20;
        if (!inPlace && !again) n += probe.optLong("romfs_bytes") + probe.optLong("romfs_files") * 64;
        JSONArray d = probe.optJSONArray("dlc");
        for (int i = 0; d != null && i < d.length(); i++) n += d.optJSONObject(i).optLong("bytes");
        return n + (600L << 20) + (2500L << 20) + (800L << 20) + (350L << 20);
    }

    private static long freeSpace(String path) {
        for (File f = new File(path); f != null; f = f.getParentFile())
            if (f.exists()) return f.getUsableSpace();
        return -1;
    }

    private static String gb(long bytes) {
        if (bytes < 0) return "?";
        return String.format(Locale.ROOT, bytes >= 10e9 ? "%.0f GB" : "%.1f GB", bytes / 1e9);
    }

    private void addMessages(JSONArray ms, boolean errorsOnly) {
        for (int i = 0; ms != null && i < ms.length(); i++) {
            JSONObject m = ms.optJSONObject(i);
            if (m == null || "prepare.input.found".equals(m.optString("key"))) continue;  // the paths: said above
            String sev = m.optString("severity");
            if (errorsOnly && !sev.equals("error")) continue;
            int color = sev.equals("error") ? Screen.RED : sev.equals("warning") ? Screen.ORANGE : Screen.GREEN_TEXT;
            String mark = sev.equals("error") ? "✕ " : sev.equals("warning") ? "! " : "✓ ";
            box.addView(Screen.text(this, mark + message(m), 15, color));
        }
    }

    private void pick(int request) {
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), request);
        } catch (Exception e) {
            box.addView(Screen.text(this, t("This phone has no folder picker.", "Este celular não tem seletor de pastas."), 15, Screen.RED));
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null || source == null) return;
        String path;
        try {
            path = Picker.pathOf(data.getData());
        } catch (Exception e) {
            return;
        }
        if (request == PICK_OUT) {
            // the folder picked when it is empty or a game folder; else a new one inside it
            File f = new File(path);
            String[] in = f.list();
            out = in != null && (in.length == 0 || prepared(f)) ? path : freeName(f);
            inPlace = false;
            showForm();
        } else if (request == PICK_DLC) {
            dlc = path;
            showForm();
            check();
        }
    }

    // ---- starting -------------------------------------------------------------------------------------------------

    private void askAndStart() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, ASK_NOTIFY);
            return;  // started once answered, whatever the answer (the notification is a help, not a need)
        }
        start();
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        if (request == ASK_NOTIFY && !PrepareService.active()) start();
    }

    private void start() {
        if (PrepareService.active() || probe == null || !probe.optBoolean("ok")) return;
        prefs.edit().putString("source", source).putString("dlc", dlc).putString("out", again ? "" : out)
                .putBoolean("in_place", inPlace).putBoolean("fps60", fps60).apply();
        StringBuilder r = new StringBuilder();
        r.append("input=").append(source).append('\n');
        if (!dlc.isEmpty()) r.append("dlc=").append(dlc).append('\n');
        r.append("out=").append(inPlace ? "" : out).append('\n');
        r.append("in_place=").append(inPlace ? "1" : "0").append('\n');
        r.append("platforms=android-arm64\n");
        r.append("fps60=").append(fps60 ? "1" : "0").append('\n');
        log.clear();
        logNext = 0;
        cancelAsked = false;
        getSystemService(NotificationManager.class).cancel(2);
        PrepareService.begin(this, r.toString());
        showRunning();
        startPolling();
    }

    // ---- running ------------------------------------------------------------------------------------------------

    private void showRunning() {
        box = Screen.page(this, t("Preparing the game", "Preparando o jogo"));
        stagesView = Screen.text(this, "", 16, Screen.BROWN);
        stagesView.setLineSpacing(0, 1.25f);
        box.addView(stagesView);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Screen.dp(this, 24)));
        percentView = Screen.text(this, "", 15, Screen.BROWN);
        box.addView(percentView);
        itemView = Screen.text(this, "", 13, Screen.TEXT);
        box.addView(itemView);
        timeView = Screen.text(this, "", 14, Screen.TEXT);
        box.addView(timeView);
        box.addView(Screen.text(this, t("You can leave the app: the notification shows how it goes. Keep the phone "
                        + "charging. If it stops half way, start again: what was done is kept.",
                "Você pode sair do app: a notificação mostra o andamento. Deixe o celular carregando. Se parar no "
                        + "meio, comece de novo: o que foi feito fica."), 14, Screen.ORANGE));
        logView = Screen.text(this, "", 11, Screen.TEXT);
        logView.setTypeface(Typeface.MONOSPACE);
        box.addView(logView);
        box.addView(Screen.button(this, cancelAsked ? t("Cancelling...", "Cancelando...") : t("Cancel", "Cancelar"),
                Screen.WHITE, v -> {
                    cancelAsked = true;
                    PrepareService.cancel();
                    ((android.widget.Button) v).setText(t("Cancelling...", "Cancelando..."));
                }));
        update();
    }

    private void startPolling() {
        if (polling) return;
        polling = true;
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (!polling) return;
                if (update()) handler.postDelayed(this, 500);
                else polling = false;
            }
        });
    }

    // The running screen from the service's phase and the native status: false once it ended (the end shown).
    private boolean update() {
        if (stagesView == null) return false;
        int phase = PrepareService.phase;
        if (phase == PrepareService.TOOLS) {
            stagesView.setText("▶ " + t("Getting the tools ready (once per app version: unpacking the compiler)",
                    "Aprontando as ferramentas (uma vez por versão do app: descompactando o compilador)"));
            bar.setIndeterminate(true);
            percentView.setText("");
            elapsed();
            return true;
        }
        JSONObject st = PrepareNative.status(logNext);
        JSONArray lines = st.optJSONArray("log");
        for (int i = 0; lines != null && i < lines.length(); i++) log.add(lines.optJSONObject(i).optString("text"));
        while (log.size() > 12) log.remove(0);
        logNext = st.optLong("log_next", logNext);
        if (st.optBoolean("finished")) {
            showFinished(st);
            return false;
        }
        if (phase == PrepareService.FINISHED) {  // ended before the preparation itself (the tools, a cancel)
            showFinished(PrepareService.result);
            return false;
        }
        if (phase == PrepareService.IDLE) {  // the process started again: nothing runs
            home();
            return false;
        }
        int current = st.optInt("stage_n");  // 1 check ... 7 finish
        StringBuilder s = new StringBuilder();
        for (int k = 0; k < STAGES.length; k++) {
            int n = k + 1;
            String mark = n < current ? "✓ " : n == current ? "▶ " : "· ";
            s.append(mark).append(stageText(STAGES[k]));
            if (n == current) {
                s.append(" — ").append(Math.round(st.optDouble("fraction") * 100)).append('%');
                if (STAGES[k].equals("compile") && st.optInt("platforms") > 1)
                    s.append(" (").append(st.optInt("platform")).append('/').append(st.optInt("platforms")).append(')');
            }
            if (k + 1 < STAGES.length) s.append('\n');
        }
        stagesView.setText(s);
        bar.setIndeterminate(false);
        bar.setProgress((int) Math.round(st.optDouble("overall") * 1000));
        percentView.setText(Math.round(st.optDouble("overall") * 100) + "%");
        itemView.setText(st.optString("item"));
        StringBuilder l = new StringBuilder();
        for (String ln : log) l.append(ln.length() > 160 ? ln.substring(0, 160) + "..." : ln).append('\n');
        logView.setText(l);
        elapsed();
        return true;
    }

    private void elapsed() {
        if (PrepareService.started == 0) return;
        long min = (System.currentTimeMillis() - PrepareService.started) / 60000;
        String time = min >= 60 ? (min / 60) + " h " + String.format(Locale.ROOT, "%02d", min % 60) + " min" : min + " min";
        timeView.setText(t("Running for ", "Rodando há ") + time);
    }

    // ---- the end ------------------------------------------------------------------------------------------------

    private void showFinished(JSONObject st) {
        polling = false;
        stagesView = null;
        if (st == null) st = new JSONObject();
        int rc = st.optInt("rc", 1);
        if (rc == 0) {
            final String folder = st.optString("out");
            box = Screen.page(this, t("The game is ready", "O jogo está pronto"));
            box.addView(Screen.text(this, t("Prepared in:", "Preparado em:") + "\n" + folder, 15, Screen.TEXT));
            box.addView(Screen.text(this, t("The extracted ExeFS and RomFS are no longer needed to play: you can "
                            + "delete them" + (inPlaceDone(folder) ? ", except the romfs folder inside the game folder."
                            : " to free the space.") ,
                    "O ExeFS e o RomFS extraídos não são mais necessários para jogar: você pode apagá-los"
                            + (inPlaceDone(folder) ? ", menos a pasta romfs dentro da pasta do jogo." : " para liberar o espaço.")),
                    14, Screen.TEXT));
            box.addView(Screen.button(this, t("Play", "Jogar"), Screen.GREEN, v -> play(folder)));
            return;
        }
        boolean cancelled = rc == 4;
        box = Screen.page(this, cancelled ? t("Preparation cancelled", "Preparação cancelada")
                                          : t("The preparation stopped", "A preparação parou"));
        if (cancelled) {
            box.addView(Screen.text(this, t("What was done is kept: start again to go on from there.",
                    "O que foi feito fica: comece de novo para continuar de onde parou."), 15, Screen.TEXT));
        } else {
            String e = st.optString("error");
            box.addView(Screen.text(this, st.optBoolean("translated") ? e : error(e), 15, Screen.RED));
            if (rc == 2) addMessages(st.optJSONArray("messages"), true);
            StringBuilder l = new StringBuilder();
            for (String ln : log) l.append(ln).append('\n');
            if (l.length() > 0) {
                TextView lv = Screen.text(this, l.toString(), 11, Screen.TEXT);
                lv.setTypeface(Typeface.MONOSPACE);
                box.addView(lv);
            }
            File inGame = new File(st.optString("out"), ".import/prepare.log"), logs = getExternalFilesDir(null);
            File whole = !st.optString("out").isEmpty() && inGame.isFile() ? inGame
                       : new File(logs != null ? logs : getFilesDir(), "prepare.log");
            if (whole.isFile())
                box.addView(Screen.text(this, t("The whole log: ", "O registro completo: ") + whole.getPath(), 13, Screen.TEXT));
        }
        final String src = prefs.getString("source", null);
        if (src != null)
            box.addView(Screen.button(this, cancelled ? t("Back", "Voltar") : t("Try again", "Tentar de novo"), Screen.GREEN, v -> {
                PrepareService.phase = PrepareService.IDLE;
                // a game folder prepared again where it is has no "out" of its own
                boolean where = prefs.getString("out", "").isEmpty() && !prefs.getBoolean("in_place", false);
                setSource(src, where ? src : null);
                showForm();
                check();
            }));
        box.addView(Screen.button(this, t("Choose another folder", "Escolher outra pasta"), Screen.BLUE, v -> {
            PrepareService.phase = PrepareService.IDLE;
            home();
        }));
    }

    private static boolean inPlaceDone(String folder) { return new File(folder, "romfs").isDirectory(); }

    // As Home does: the folder saved, and the app opened into it (Home loads the game and starts it).
    private void play(String folder) {
        try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "data_root.txt"))) {
            o.write((folder + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            // the service saved it already, or Home asks for the folder
        }
        PrepareService.phase = PrepareService.IDLE;
        getSystemService(NotificationManager.class).cancel(2);
        Intent h = new Intent(this, Home.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(h);
        finish();
    }

    // ---- words ------------------------------------------------------------------------------------------------------

    static String stageText(String stage) {
        switch (stage) {
            case "check": return t("Checking the files", "Verificando os arquivos");
            case "exefs": return t("Copying the ExeFS", "Copiando o ExeFS");
            case "recompile": return t("Recompiling the game's code", "Recompilando o código do jogo");
            case "shaders": return t("Converting the shaders", "Convertendo os shaders");
            case "packs": return t("Packing the RomFS", "Empacotando o RomFS");
            case "compile": return t("Compiling the game's code", "Compilando o código do jogo");
            case "finish": return t("Finishing", "Finalizando");
            case "done": return t("Done", "Pronto");
            default: return t("Getting ready", "Preparando");
        }
    }

    // A refusal or a failure (prepare.cpp's English) with its first words in the phone's language when it is one the
    // app knows; the details (paths, sizes, the tools' output) as they are.
    private static String error(String e) {
        if (!Screen.pt()) return e;
        Matcher m = Pattern.compile("^not enough free space for the game folder: (.*) needed, (.*) free on (.*)$",
                Pattern.DOTALL).matcher(e);
        if (m.find())
            return "Não há espaço livre para a pasta do jogo: " + m.group(1) + " necessários, " + m.group(2)
                    + " livres em " + m.group(3);
        m = Pattern.compile("^(.*) holds other things: choose an empty folder for the game \\(or one prepared before\\)$",
                Pattern.DOTALL).matcher(e);
        if (m.find()) return m.group(1) + " tem outras coisas: escolha uma pasta vazia para o jogo (ou uma preparada antes)";
        if (e.equals("cancelled")) return "cancelado";
        return "A preparação parou:\n" + e;
    }

    // A finding of the check (prepare/io/inputs.cpp's kTexts), in Portuguese on a phone set to it.
    static String message(JSONObject m) {
        String text = m.optString("text");
        if (!Screen.pt()) return text;
        String tmpl = PT.get(m.optString("key"));
        if (tmpl == null) return text;
        JSONArray args = m.optJSONArray("args");
        StringBuilder s = new StringBuilder();
        int k = 0;
        for (int i = 0; i < tmpl.length(); i++) {
            if (tmpl.charAt(i) == '{' && i + 1 < tmpl.length() && tmpl.charAt(i + 1) == '}') {
                if (args != null && k < args.length()) s.append(args.optString(k));
                k++;
                i++;
            } else {
                s.append(tmpl.charAt(i));
            }
        }
        return s.toString();
    }

    private static final Map<String, String> PT = new HashMap<>();
    static {
        String extract = " (Ryujinx: clique com o botão direito no jogo, Extract Data; yuzu ou Eden: Dump RomFS, e Dump "
                + "ExeFS nas configurações de depuração)";
        PT.put("prepare.input.found", "ExeFS: {} · RomFS: {}");
        PT.put("prepare.input.missing", "{} não existe");
        PT.put("prepare.input.not_folder", "{} é um arquivo: escolha a pasta em que o seu emulador extraiu");
        PT.put("prepare.input.container", "{} é um pacote do jogo (NSP, XCI, NSZ, XCZ ou NCA). O ACNHRecomp não lê "
                + "esses arquivos: extraia o ExeFS e o RomFS com o seu emulador" + extract + " e escolha as pastas "
                + "que ele criou");
        PT.put("prepare.input.container_ignored", "{} é um pacote do jogo (NSP, XCI, NSZ, XCZ ou NCA): o ACNHRecomp "
                + "nunca lê esses arquivos e o deixa onde está. O jogo é preparado só a partir do ExeFS e do RomFS "
                + "extraídos" + extract);
        PT.put("prepare.input.layout", "{} não tem um ExeFS (main, sdk, subsdk0, main.npdm), nem um RomFS (Shader, "
                + "Bcsv, ...), nem as pastas exefs e romfs");
        PT.put("prepare.input.no_exefs", "Nenhum ExeFS foi encontrado: escolha a pasta que tem também o ExeFS "
                + "extraído (main, sdk, subsdk0, main.npdm), na pasta exefs");
        PT.put("prepare.input.no_romfs", "Nenhum RomFS foi encontrado: escolha a pasta que tem também o RomFS "
                + "extraído (Shader, Bcsv, ...), na pasta romfs");
        PT.put("prepare.input.pick_pair", "Escolha uma pasta de ExeFS e uma de RomFS: {} e {} não são uma de cada");
        PT.put("prepare.input.too_many", "Escolha uma pasta (com o ExeFS e o RomFS) ou duas (o ExeFS, o RomFS); "
                + "foram escolhidas {}");
        PT.put("prepare.exefs.missing", "O ExeFS não tem {}: extraia o ExeFS de novo");
        PT.put("prepare.exefs.unreadable", "Não foi possível ler o ExeFS {}");
        PT.put("prepare.exefs.not_nso", "O ExeFS {} não é um dos executáveis do jogo (sem cabeçalho NSO)");
        PT.put("prepare.exefs.truncated", "O ExeFS {} está incompleto (uma cópia que parou no meio?): extraia o "
                + "ExeFS de novo");
        PT.put("prepare.exefs.bad_hash", "ExeFS {}: o segmento {} falha na verificação SHA-256 (um arquivo "
                + "danificado): extraia o ExeFS de novo");
        PT.put("prepare.exefs.corrupt", "O ExeFS {} está danificado ({}): extraia o ExeFS de novo");
        PT.put("prepare.exefs.optional_bad", "O ExeFS {} está danificado ({}); o jogo não precisa dele");
        PT.put("prepare.exefs.npdm_missing", "O ExeFS não tem main.npdm: extraia o ExeFS de novo");
        PT.put("prepare.exefs.npdm_invalid", "O main.npdm do ExeFS não é uma descrição de programa (sem META e ACI0)");
        PT.put("prepare.exefs.other_game", "Este ExeFS é de outro programa ({}), não do Animal Crossing: New "
                + "Horizons (01006F8002326000)");
        PT.put("prepare.exefs.version", "Animal Crossing: New Horizons {}");
        PT.put("prepare.exefs.version_302", "Versão 3.0.2: ainda aceita para as pastas do jogo feitas com ela, mas o "
                + "ACNHRecomp é feito para a atualização 3.0.3. Instale a atualização 3.0.3 no seu emulador e extraia "
                + "o ExeFS e o RomFS de novo");
        PT.put("prepare.exefs.needs_303", "Este ExeFS é da versão {}: o ACNHRecomp precisa da atualização 3.0.3. "
                + "Instale a atualização 3.0.3 no seu emulador e extraia o ExeFS e o RomFS de novo (com a "
                + "atualização aplicada)");
        PT.put("prepare.exefs.unknown_build", "O main deste ExeFS (build id {}) não é uma versão do Animal Crossing: "
                + "New Horizons que o ACNHRecomp conhece: ele precisa da atualização 3.0.3. Instale-a no seu emulador "
                + "e extraia o ExeFS e o RomFS de novo");
        PT.put("prepare.romfs.missing", "A pasta do RomFS {} não existe");
        PT.put("prepare.romfs.unreadable", "Não foi possível ler o RomFS: {}");
        PT.put("prepare.romfs.empty", "A pasta do RomFS {} está vazia");
        PT.put("prepare.romfs.skeleton", "O RomFS tem as pastas, mas não os dados dos arquivos ({} de {} arquivos "
                + "estão vazios): um dump \"esqueleto\". Faça o dump do RomFS de novo, escolhendo Full");
        PT.put("prepare.romfs.not_acnh", "{} não é o RomFS do Animal Crossing: New Horizons (não tem Shader, Bcsv, "
                + "Message, ...)");
        PT.put("prepare.romfs.incomplete", "Faltam no RomFS pastas de que o jogo precisa ({}): extraia o RomFS de novo");
        PT.put("prepare.romfs.key_file", "O RomFS não tem {} (ou está vazio): ele pode estar incompleto");
        PT.put("prepare.romfs.few_files", "O RomFS tem {} arquivos ({}), e um completo cerca de 103.000 (10,5 GB): "
                + "ele pode estar incompleto");
        PT.put("prepare.romfs.summary", "RomFS: {} arquivos, {}");
        PT.put("prepare.dlc.missing", "A pasta do conteúdo adicional {} não existe");
        PT.put("prepare.dlc.unreadable", "Não foi possível ler a pasta do conteúdo adicional: {}");
        PT.put("prepare.dlc.empty", "A pasta do conteúdo adicional {} está vazia");
        PT.put("prepare.dlc.skeleton", "O RomFS do conteúdo adicional em {} tem as pastas, mas não os dados dos "
                + "arquivos (um dump \"esqueleto\"): faça o dump de novo, escolhendo Full");
        PT.put("prepare.dlc.is_game", "{} é o conteúdo do próprio jogo, não o de um conteúdo adicional");
        PT.put("prepare.dlc.other_title", "{} é o conteúdo de outro título ({}), não um conteúdo adicional do Animal "
                + "Crossing: New Horizons");
        PT.put("prepare.dlc.unknown_index", "Pelo nome da pasta não dá para saber qual conteúdo adicional {} tem: ele "
                + "é tomado como o conteúdo adicional 1 (Happy Home Paradise, o único pago do jogo). Se for outro, dê "
                + "à pasta o nome do título dele (01006F8002327xxx)");
        PT.put("prepare.dlc.duplicate", "{} e {} são ambos o conteúdo adicional {}");
        PT.put("prepare.dlc.summary", "Conteúdo adicional {}: {} arquivos, {}");
    }
}
