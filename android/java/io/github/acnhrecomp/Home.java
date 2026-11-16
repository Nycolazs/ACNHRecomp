// The app's first screen: asks for the game folder, checks it and starts the game (android.app.NativeActivity,
// lib<game>.so). The APK holds no game: the folder is one prepared from the player's own copy, on the phone itself
// (Prepare.java, from the ExeFS and RomFS they extracted with their emulator), by ACNHRecomp on a computer or by
// ACNHRecomp-Importer (game/android-arm64/libacnh_game.so, romfs.pak, spirv.pak, ...; docs/ANDROID.md). A folder
// holding exefs/ and romfs/ that is no game folder yet opens Prepare. The folder chosen is kept in
// <files>/data_root.txt, which platform_android.cpp reads; next time the game starts right away. The launcher icon's
// shortcut "Change game folder" (res/xml/shortcuts.xml) asks again.
package io.github.acnhrecomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import static io.github.acnhrecomp.Screen.t;

public class Home extends Activity {
    static {
        System.loadLibrary("acnh");
    }

    // platform_android.cpp: "" when the game library in `folder` loads (copied into `files` first), else why not.
    private static native String nativeLoadGame(String folder, String files);

    private static final int PICK = 1;
    private static final String GAME_LIB = "libacnh_game.so";

    private LinearLayout box;
    private TextView message;
    private String waitingForAccess;  // a folder picked before the all-files permission was given
    private boolean busy;

    // The app started again in a new process (the chosen folder is saved: it opens straight into it). Android keeps a
    // closed app's process, with the game of the last folder loaded, so "close and open again" was not enough.
    private void restartProcess() {
        Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (i != null) {
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
        }
        finishAffinity();
        // as platform_android.cpp's restart_app (_exit): no atexit handlers or static destructors with native threads
        // still running
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String error = getIntent().getStringExtra("error");
        if ("restart".equals(error)) {  // another folder than the game loaded in this process: a new process takes it
            restartProcess();
            return;
        }
        if (PrepareService.active()) {  // the game is being prepared: its screen
            startActivity(new Intent(this, Prepare.class));
            finish();
            return;
        }
        boolean ask = getIntent().getBooleanExtra("pick", false) || error != null;
        String saved = ask ? "" : readSaved();
        if (!saved.isEmpty() && hasPackage(new File(saved)) && canRead(saved)) {
            check(saved, true);  // the folder of last time: straight into the game
            return;
        }
        show(error != null ? explain(error) : null);
        if (error != null) offerAgain(readSaved(), error);
        if (!ask && found().isEmpty()) pick();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // the game being prepared, or prepared and its end not seen yet: its screen (Android can bring this task to
        // the front with Home on top when the preparation's notification is tapped)
        if (PrepareService.active() || (PrepareService.phase == PrepareService.FINISHED && PrepareService.result != null)) {
            startActivity(new Intent(this, Prepare.class));
            return;
        }
        if (waitingForAccess != null && Environment.isExternalStorageManager()) {
            String f = waitingForAccess;
            waitingForAccess = null;
            check(f, false);
        }
    }

    // ---- the screen -------------------------------------------------------------------------------------------

    private void show(String note) {
        box = Screen.page(this, "ACNH Recomp");
        box.addView(Screen.text(this, t("Choose the game folder: one prepared before (on this phone or on a computer), "
                        + "or the folder holding the exefs and romfs you extracted from your own copy of the game with "
                        + "your emulator, which the app then prepares here.",
                "Escolha a pasta do jogo: uma preparada antes (neste celular ou num computador), ou a pasta com o "
                        + "exefs e o romfs que você extraiu da sua cópia do jogo com o seu emulador, que o app então "
                        + "prepara aqui."), 16, Screen.TEXT));
        message = Screen.text(this, note == null ? "" : note, 15, Screen.RED);
        message.setVisibility(note == null ? View.GONE : View.VISIBLE);
        box.addView(message);

        for (final String f : found())
            box.addView(Screen.button(this, t("Use the game found", "Usar o jogo encontrado") + " ("
                            + (f.startsWith("/storage/emulated") ? t("phone", "celular") : t("memory card", "cartão de memória"))
                            + ")", Screen.GREEN, v -> check(f, false)));
        box.addView(Screen.button(this, t("Choose the game folder", "Escolher a pasta do jogo"), Screen.BLUE, v -> pick()));
    }

    private void say(String text, boolean error) {
        if (message == null) return;
        message.setTextColor(error ? Screen.RED : Screen.GREEN_TEXT);
        message.setText(text);
        message.setVisibility(View.VISIBLE);
    }

    private String explain(String why) {
        if (why.startsWith("abi"))
            return t("The game in this folder was prepared for another version of the app. Prepare it again (on this "
                            + "phone, below, or on a computer) or install the matching app.",
                    "O jogo desta pasta foi preparado para outra versão do app. Prepare de novo (neste celular, "
                            + "abaixo, ou num computador) ou instale o app da mesma versão.");
        if (why.startsWith("missing"))
            return t("This folder has neither a prepared game (" + GAME_LIB + ") nor the game's extracted files. With "
                            + "your emulator, extract the ExeFS and RomFS of your own copy of the game, with update "
                            + "3.0.3 (Ryujinx: right-click the game, Extract Data; yuzu or Eden: Dump RomFS, and Dump "
                            + "ExeFS in the debug settings), into one folder as exefs and romfs, copy that folder to "
                            + "the phone and choose it here: the app prepares the game from it. A computer does that "
                            + "faster: ACNHRecomp's launcher there prepares a folder for Android too.",
                    "Esta pasta não tem nem o jogo preparado (" + GAME_LIB + ") nem os arquivos extraídos do jogo. "
                            + "Com o seu emulador, extraia o ExeFS e o RomFS da sua cópia do jogo, com a atualização "
                            + "3.0.3 (Ryujinx: botão direito no jogo, Extract Data; yuzu ou Eden: Dump RomFS, e Dump "
                            + "ExeFS nas configurações de depuração), numa pasta como exefs e romfs, copie essa pasta "
                            + "para o celular e escolha-a aqui: o app prepara o jogo a partir dela. Um computador faz "
                            + "isso mais rápido: o launcher do ACNHRecomp nele também prepara uma pasta para Android.");
        if (why.contains("cannot locate symbol"))  // a library from ACNHRecomp-Importer before 1.0.4 (no DT_NEEDED)
            return t("The app can't load the game in this folder. Usually it was prepared by an older "
                            + "ACNHRecomp-Importer (before 1.0.4): prepare it again (on this phone, below, or on a "
                            + "computer), which keeps what was extracted and builds the library again. If it was "
                            + "prepared by something newer, update this app.\n\n" + why,
                    "O app não consegue carregar o jogo desta pasta. Normalmente ela foi preparada por um "
                            + "ACNHRecomp-Importer antigo (anterior ao 1.0.4): prepare de novo (neste celular, abaixo, "
                            + "ou num computador), o que reaproveita o que foi extraído e refaz a biblioteca. Se foi "
                            + "preparada por algo mais novo, atualize este app.\n\n" + why);
        if (why.equals("restart"))
            return t("Close the app and open it again to use another folder.",
                    "Feche o app e abra de novo para usar outra pasta.");
        return why;
    }

    // ---- folders ----------------------------------------------------------------------------------------------

    // The game's files as the player's emulator extracted them, not prepared yet: exefs/ and romfs/ (any case; or
    // extract/romfs/), or yuzu's and Eden's dump folder of the game (01006F8002326000/). prepare/io/inputs.h says it.
    static boolean extracted(File f) {
        if (f.getName().equalsIgnoreCase("01006F8002326000")) return true;
        boolean exefs = false, romfs = false;
        File[] in = f.listFiles();
        if (in == null) return false;
        for (File c : in) {
            if (!c.isDirectory()) continue;
            String n = c.getName();
            if (n.equalsIgnoreCase("exefs")) exefs = true;
            else if (n.equalsIgnoreCase("romfs") || n.equalsIgnoreCase("01006F8002326000")) romfs = true;
            else if (n.equalsIgnoreCase("extract") && new File(c, "romfs").isDirectory()) romfs = true;
            if (n.equalsIgnoreCase("01006F8002326000")) exefs = true;
        }
        return exefs && romfs;
    }

    private void openPrepare(String folder, String again) {
        Intent i = new Intent(this, Prepare.class).putExtra("source", folder);
        if (again != null) i.putExtra("out", again);
        startActivity(i);
    }

    // A game folder this app cannot load (prepared for another version, or by an Importer before 1.0.4) but holding
    // the game's ExeFS: prepared again here, where it is.
    private void offerAgain(final String folder, String why) {
        if (folder.isEmpty() || !(why.startsWith("abi") || why.contains("cannot locate symbol"))
                || !new File(folder, "exefs").isDirectory() || box == null)
            return;
        box.addView(Screen.button(this, t("Prepare it again on this phone", "Preparar de novo neste celular"),
                Screen.GREEN, v -> openPrepare(folder, folder)));
    }

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
            if (extracted(new File(folder))) {  // the game's extracted files: prepared on the phone
                if (message == null) show(null);
                openPrepare(folder, null);
                return;
            }
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
                if (!why.isEmpty() && !why.equals("restart")) {
                    if (message == null) show(null);
                    say(explain(why), true);
                    offerAgain(folder, why);
                    return;
                }
                boolean saved = true;
                try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "data_root.txt"))) {
                    o.write((folder + "\n").getBytes(StandardCharsets.UTF_8));
                } catch (Exception e) {
                    saved = false;  // the game still starts; next time the folder is asked for again
                }
                if (why.equals("restart")) {  // this process has another folder's game loaded: a new one takes this folder
                    if (saved) restartProcess();
                    else say(explain(why), true);  // not saved: a new process would not know the folder
                    return;
                }
                Intent game = new Intent();
                game.setClassName(getPackageName(), "android.app.NativeActivity");
                startActivity(game);
                finish();
            });
        }).start();
    }
}
