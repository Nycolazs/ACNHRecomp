// The settings screen's folder picker and "open folder" while the game runs (the Saves page: import, export, the
// save folder). NativeActivity cannot get an activity result, so platform_android.cpp starts this one: with "out" it
// asks for a folder (the system picker) and writes its path to that file ("" when cancelled), which pick_result()
// reads; with "open" it shows that folder in a file manager; with "driver" (a game folder's drivers/) and "out" it asks
// for a GPU driver, a .so or the .zip emulators' drivers come in, and copies its .so there ("ok:<name>",
// "error:<why>", or "" when cancelled).
package io.github.acnhrecomp;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.nio.charset.StandardCharsets;

public class Picker extends Activity {
    private static final int PICK = 1, PICK_DRIVER = 2;
    private static final long MAX_DRIVER = 256L << 20;  // a Turnip build is ~20 MB
    private String out;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String open = getIntent().getStringExtra("open");
        if (open != null) {
            openFolder(open);
            finish();
            return;
        }
        out = getIntent().getStringExtra("out");
        if (state != null) return;  // re-created while the picker is up: its result still comes here
        if (getIntent().getStringExtra("driver") != null) {
            try {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                startActivityForResult(i, PICK_DRIVER);
            } catch (Exception e) {
                write("error:" + e);
                finish();
            }
            return;
        }
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK);
        } catch (Exception e) {
            write("");
            finish();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_DRIVER) {
            boolean picked = result == RESULT_OK && data != null && data.getData() != null;
            write(picked ? importDriver(data.getData()) : "");
            finish();
            return;
        }
        String path = "";
        if (request == PICK && result == RESULT_OK && data != null && data.getData() != null) {
            try {
                path = pathOf(data.getData());
            } catch (Exception e) {
                path = "";
            }
        }
        write(path);
        finish();
    }

    private void write(String path) {
        if (out == null) return;
        File f = new File(out), tmp = new File(out + ".tmp");
        try (FileOutputStream o = new FileOutputStream(tmp)) {
            o.write((path + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            return;
        }
        tmp.renameTo(f);  // whole, or not at all: the game polls for it
    }

    // The picked driver into drivers/, alone there (the game loads the first .so in it, platform_android.cpp): a .so as
    // it is, or the first .so inside a .zip. Written to a temporary file first, so a failed copy leaves the old one.
    private String importDriver(Uri uri) {
        File dir = new File(getIntent().getStringExtra("driver"));
        if (!dir.isDirectory() && !dir.mkdirs()) return "error:cannot create " + dir;
        String name = displayName(uri);
        File tmp = new File(dir, "import.tmp");
        String outName = null;
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) return "error:cannot read " + name;
            if (name.toLowerCase(Locale.ROOT).endsWith(".so")) {
                copy(in, tmp);
                outName = name;
            } else {
                try (ZipInputStream z = new ZipInputStream(in)) {
                    for (ZipEntry e; (e = z.getNextEntry()) != null;) {
                        String n = e.getName(), base = n.substring(n.lastIndexOf('/') + 1);
                        if (!e.isDirectory() && base.toLowerCase(Locale.ROOT).endsWith(".so")) {
                            copy(z, tmp);
                            outName = base;
                            break;
                        }
                    }
                }
                if (outName == null) return "error:no .so file in " + name;
            }
        } catch (Exception e) {
            tmp.delete();
            return "error:" + e.getMessage();
        }
        if (outName.isEmpty() || outName.contains("/") || outName.contains("..")) outName = "driver.so";
        File[] old = dir.listFiles();
        if (old != null)
            for (File f : old)
                if (f.getName().toLowerCase(Locale.ROOT).endsWith(".so")) f.delete();
        File dst = new File(dir, outName);
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            return "error:cannot write " + dst;
        }
        return "ok:" + outName;
    }
    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) {
                String n = c.getString(0);
                return n.substring(n.lastIndexOf('/') + 1);
            }
        } catch (Exception e) {
            // the name stays unknown: treated as a .zip
        }
        return "driver.zip";
    }
    private static void copy(InputStream in, File to) throws Exception {
        byte[] buf = new byte[1 << 16];
        long total = 0;
        try (OutputStream o = new FileOutputStream(to)) {
            for (int n; (n = in.read(buf)) > 0;) {
                total += n;
                if (total > MAX_DRIVER) throw new Exception("the file is larger than a driver");
                o.write(buf, 0, n);
            }
        }
    }
    // As Home.pathOf: "primary:Games/ACNH" is the phone's storage, "7EFF-EC74:ACNH" a memory card.
    static String pathOf(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        String volume = colon < 0 ? id : id.substring(0, colon), rest = colon < 0 ? "" : id.substring(colon + 1);
        String base = volume.equals("primary") ? Environment.getExternalStorageDirectory().getAbsolutePath()
                                               : "/storage/" + volume;
        return rest.isEmpty() ? base : base + "/" + rest;
    }

    // The folder in the system's file manager (DocumentsUI), when the path is on shared storage; else (the app's own
    // folder, which no other app may open) its path in a toast.
    private void openFolder(String path) {
        String root = Environment.getExternalStorageDirectory().getAbsolutePath();
        String id = null;
        if (path.equals(root)) id = "primary:";
        else if (path.startsWith(root + "/")) id = "primary:" + path.substring(root.length() + 1);
        else if (path.startsWith("/storage/")) {
            String rest = path.substring("/storage/".length());
            int slash = rest.indexOf('/');
            id = slash < 0 ? rest + ":" : rest.substring(0, slash) + ":" + rest.substring(slash + 1);
        }
        if (id != null) {
            Uri uri = DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", id);
            for (String type : new String[] {DocumentsContract.Document.MIME_TYPE_DIR, null}) {
                try {
                    Intent i = new Intent(Intent.ACTION_VIEW);
                    if (type != null) i.setDataAndType(uri, type);
                    else i.setData(uri);
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    startActivity(i);
                    return;
                } catch (Exception e) {
                    // next
                }
            }
        }
        Toast.makeText(getApplicationContext(), path, Toast.LENGTH_LONG).show();
    }
}
