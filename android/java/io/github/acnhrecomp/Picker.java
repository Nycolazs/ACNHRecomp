// The settings screen's folder picker and "open folder" while the game runs (the Saves page: import, export, the
// save folder). NativeActivity cannot get an activity result, so platform_android.cpp starts this one: with "out" it
// asks for a folder (the system picker) and writes its path to that file ("" when cancelled), which pick_result()
// reads; with "open" it shows that folder in a file manager.
package io.github.acnhrecomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public class Picker extends Activity {
    private static final int PICK = 1;
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
