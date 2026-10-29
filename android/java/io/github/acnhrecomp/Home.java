// The app's first screen: asks for the game folder, checks it and starts the game (android.app.NativeActivity,
// lib<game>.so). The APK holds no game: the folder is the one the player's computer prepared from their own copy
// (tools/android_install.sh: the recompiled code as libacnh_game.so, romfs.pak, spirv.pak, ...; docs/ANDROID.md).
// The folder chosen is kept in <files>/data_root.txt, which platform_android.cpp reads; next time the game starts
// right away. The launcher icon's shortcut "Change game folder" (res/xml/shortcuts.xml) asks again.
package io.github.acnhrecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class Home extends Activity {
    static {
        System.loadLibrary("acnh");
    }

    // platform_android.cpp: "" when the game library in `folder` loads (copied into `files` first), else why not.
    private static native String nativeLoadGame(String folder, String files);

    private static final int PICK = 1;
    private static final String GAME_LIB = "libacnh_game.so";

    private final boolean pt = Locale.getDefault().getLanguage().equals("pt");
    private LinearLayout box;
    private TextView message;
    private String waitingForAccess;  // a folder picked before the all-files permission was given
    private boolean busy;

    private String t(String en, String ptBr) { return pt ? ptBr : en; }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String error = getIntent().getStringExtra("error");
        boolean ask = getIntent().getBooleanExtra("pick", false) || error != null;
        String saved = ask ? "" : readSaved();
        if (!saved.isEmpty() && hasPackage(new File(saved)) && canRead(saved)) {
            check(saved, true);  // the folder of last time: straight into the game
            return;
        }
        show(error != null ? explain(error) : null);
        if (!ask && found().isEmpty()) pick();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (waitingForAccess != null && Environment.isExternalStorageManager()) {
            String f = waitingForAccess;
            waitingForAccess = null;
            check(f, false);
        }
    }

    // ---- the screen -------------------------------------------------------------------------------------------

    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private Button button(String text, int color, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(17);
        b.setTextColor(color == Color.WHITE ? Color.rgb(92, 70, 50) : Color.WHITE);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(26));
        b.setBackground(bg);
        b.setPadding(dp(22), dp(10), dp(22), dp(10));
        b.setOnClickListener(click);
        b.setMinHeight(dp(54));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        b.setLayoutParams(lp);
        return b;
    }

    private void show(String note) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        GradientDrawable sky = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] {Color.rgb(160, 222, 200), Color.rgb(196, 230, 160)});
        scroll.setBackground(sky);
        LinearLayout outer = new LinearLayout(this);
        outer.setGravity(Gravity.CENTER);
        outer.setPadding(dp(24), dp(24), dp(24), dp(24));
        box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(28), dp(24), dp(28), dp(24));
        GradientDrawable paper = new GradientDrawable();
        paper.setColor(Color.rgb(255, 250, 232));
        paper.setCornerRadius(dp(28));
        box.setBackground(paper);
        outer.addView(box, new LinearLayout.LayoutParams(Math.min(dp(560), getResources().getDisplayMetrics().widthPixels - dp(48)),
                LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(outer);

        TextView title = new TextView(this);
        title.setText("ACNH Recomp");
        title.setTextSize(26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(Color.rgb(92, 70, 50));
        box.addView(title);
        TextView text = new TextView(this);
        text.setText(t("Choose the game folder: the one your computer prepared from your own copy of the game "
                        + "(with libacnh_game.so, romfs.pak and spirv.pak inside).",
                "Escolha a pasta do jogo: a que o seu computador preparou a partir da sua cópia do jogo "
                        + "(com libacnh_game.so, romfs.pak e spirv.pak dentro)."));
        text.setTextSize(16);
        text.setTextColor(Color.rgb(120, 100, 80));
        text.setPadding(0, dp(8), 0, dp(8));
        box.addView(text);
        message = new TextView(this);
        message.setTextSize(15);
        message.setTextColor(Color.rgb(190, 80, 80));
        message.setVisibility(note == null ? View.GONE : View.VISIBLE);
        if (note != null) message.setText(note);
        box.addView(message);

        for (final String f : found())
            box.addView(button(t("Use the game found", "Usar o jogo encontrado") + " ("
                            + (f.startsWith("/storage/emulated") ? t("phone", "celular") : t("memory card", "cartão de memória"))
                            + ")", Color.rgb(90, 170, 90), v -> check(f, false)));
        box.addView(button(t("Choose the game folder", "Escolher a pasta do jogo"), Color.rgb(70, 140, 210), v -> pick()));
        setContentView(scroll);
    }

    private void say(String text, boolean error) {
        if (message == null) return;
        message.setTextColor(error ? Color.rgb(190, 80, 80) : Color.rgb(70, 140, 60));
        message.setText(text);
        message.setVisibility(View.VISIBLE);
    }

    private String explain(String why) {
        if (why.startsWith("abi"))
            return t("The game in this folder was prepared for another version of the app. Prepare it again on the "
                            + "computer (tools/android_install.sh) or install the matching app.",
                    "O jogo desta pasta foi preparado para outra versão do app. Prepare de novo no computador "
                            + "(tools/android_install.sh) ou instale o app da mesma versão.");
        if (why.startsWith("missing"))
            return t("This folder has no prepared game (" + GAME_LIB + "). Prepare your game with ACNHRecomp-Importer "
                            + "(download it on our Discord: discord.gg/VJW9Dtdu7a), then on the computer with "
                            + "tools/android_install.sh.",
                    "Esta pasta não tem o jogo preparado (" + GAME_LIB + "). Prepare o seu jogo com o "
                            + "ACNHRecomp-Importer (baixe no nosso Discord: discord.gg/VJW9Dtdu7a) e depois no "
                            + "computador com tools/android_install.sh.");
        if (why.equals("restart"))
            return t("Close the app and open it again to use another folder.",
                    "Feche o app e abra de novo para usar outra pasta.");
        return why;
    }

    // ---- folders ----------------------------------------------------------------------------------------------

    private static boolean hasPackage(File f) {
        // the game folder ACNHRecomp-Importer prepares (game/android-arm64/), or one tools/android_install.sh filled
        return (new File(f, "game/android-arm64/" + GAME_LIB).isFile() || new File(f, GAME_LIB).isFile())
                && new File(f, "analysis/main.bin").isFile();
    }

    // The app's own folders (phone and memory card) where tools/android_install.sh puts the game: readable without
    // any permission, and not offered by the system's folder picker.
    private List<String> found() {
        List<String> out = new ArrayList<>();
        for (File f : getExternalFilesDirs(null))
            if (f != null && hasPackage(f)) out.add(f.getAbsolutePath());
        return out;
    }

    private boolean ownFolder(String path) {
        for (File f : getExternalFilesDirs(null))
            if (f != null && path.startsWith(f.getAbsolutePath())) return true;
        return false;
    }

    private boolean canRead(String path) { return ownFolder(path) || Environment.isExternalStorageManager(); }

    private String readSaved() {
        try {
            return new String(Files.readAllBytes(new File(getFilesDir(), "data_root.txt").toPath()),
                    StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    private void pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        try {
            startActivityForResult(i, PICK);
        } catch (Exception e) {
            say(t("This phone has no folder picker.", "Este celular não tem seletor de pastas."), true);
        }
    }

    // A folder from the system picker as a path: "primary:Games/ACNH" is the phone's storage, "7EFF-EC74:ACNH" a
    // memory card (/storage/7EFF-EC74).
    private static String pathOf(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        String volume = colon < 0 ? id : id.substring(0, colon), rest = colon < 0 ? "" : id.substring(colon + 1);
        String base = volume.equals("primary") ? Environment.getExternalStorageDirectory().getAbsolutePath()
                                               : "/storage/" + volume;
        return rest.isEmpty() ? base : base + "/" + rest;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        String path = pathOf(data.getData());
        if (message == null) show(null);
        if (!canRead(path)) {
            // Reading the game's files from a folder of the phone's storage needs the all-files permission.
            waitingForAccess = path;
            say(t("Allow access to all files for ACNH Recomp, then come back.",
                    "Permita o acesso a todos os arquivos para o ACNH Recomp e volte."), false);
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(i);
            return;
        }
        check(path, false);
    }

    // Loads the game library from the folder (in the background: the first time it is copied, a few seconds) and
    // starts the game, or says why not.
    private void check(final String folder, final boolean quiet) {
        if (busy) return;
        if (!hasPackage(new File(folder))) {
            if (message == null) show(null);
            say(explain("missing") + "\n" + folder, true);
            return;
        }
        busy = true;
        if (!quiet) {
            if (message == null) show(null);
            say(t("Preparing the game...", "Preparando o jogo..."), false);
            ProgressBar p = new ProgressBar(this);
            p.setTag("progress");
            box.addView(p);
        }
        new Thread(() -> {
            final String why = nativeLoadGame(folder, getFilesDir().getAbsolutePath());
            runOnUiThread(() -> {
                busy = false;
                if (box != null) {
                    View p = box.findViewWithTag("progress");
                    if (p != null) box.removeView(p);
                }
                if (!why.isEmpty()) {
                    if (message == null) show(null);
                    say(explain(why), true);
                    return;
                }
                try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "data_root.txt"))) {
                    o.write((folder + "\n").getBytes(StandardCharsets.UTF_8));
                } catch (Exception e) {
                    // the game still starts; next time the folder is asked for again
                }
                Intent game = new Intent();
                game.setClassName(getPackageName(), "android.app.NativeActivity");
                startActivity(game);
                finish();
            });
        }).start();
    }
}
