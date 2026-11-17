// The phone's keyboard for the settings screen's text fields while the game runs (the Online page: a friend's address,
// a friend code). NativeActivity's own view is not a text editor and never has the focus, so the system keyboard did
// not show for it (Discord, One UI). platform_android.cpp adds this 1 x 1 view over it (it draws nothing), shows the
// keyboard on it while a field is edited and hides it after; what is typed goes to the field through nativeText and
// nativeKey, which platform_android.cpp registers.
package io.github.acnhrecomp;

import android.content.Context;
import android.os.Build;
import android.text.InputType;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.nio.charset.StandardCharsets;

public class TextInput extends View {
    private static native void nativeText(byte[] utf8);  // characters typed, at the field's cursor
    private static native void nativeKey(int keyCode);   // KeyEvent.KEYCODE_DEL, KEYCODE_FORWARD_DEL or KEYCODE_ENTER

    // UTF-8 bytes, not a String: JNI hands a String's text over as modified UTF-8 (emoji as two surrogates).
    static void type(String s) {
        if (!s.isEmpty()) nativeText(s.getBytes(StandardCharsets.UTF_8));
    }

    public TextInput(Context context) {
        super(context);
    }

    // Called by platform_android.cpp on the UI thread.
    public void show() {
        if (Build.VERSION.SDK_INT >= 30 && hasFocus()) {  // already up (a tap on the field it is typing in)
            WindowInsets in = getRootWindowInsets();
            if (in != null && in.isVisible(WindowInsets.Type.ime())) return;
        }
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
        final InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm == null) return;
        if (imm.showSoftInput(this, 0)) return;
        // Not yet the view the keyboard serves (the focus moved just now): again once the focus change is through.
        post(new Runnable() {
            @Override
            public void run() {
                if (imm.showSoftInput(TextInput.this, 0)) return;
                if (Build.VERSION.SDK_INT >= 30) {
                    WindowInsetsController c = getWindowInsetsController();
                    if (c != null) c.show(WindowInsets.Type.ime());
                }
            }
        });
    }

    public void hide() {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) imm.hideSoftInputFromWindow(getWindowToken(), 0);
        // Not focusable again until the next field: the keyboard must not come back by itself (the app's return).
        setFocusable(false);
        setFocusableInTouchMode(false);
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo info) {
        // Plain characters, no suggestions or corrections (addresses, codes); the keyboard below the picture, never
        // the full-screen edit box keyboards use in landscape, which would hide the game.
        info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        info.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        return new Connection(this);
    }

    // A physical keyboard's keys reach the focused view (platform_android_input.cpp leaves them to it).
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        return key(event) || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        return key(event) || super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onKeyMultiple(int keyCode, int count, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN && event.getCharacters() != null) {
            type(event.getCharacters());
            return true;
        }
        return super.onKeyMultiple(keyCode, count, event);
    }

    // The key code nativeKey takes for this key, else 0.
    static int editKey(KeyEvent e) {
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_DEL:
            case KeyEvent.KEYCODE_FORWARD_DEL:
            case KeyEvent.KEYCODE_ENTER:
                return e.getKeyCode();
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                return KeyEvent.KEYCODE_ENTER;
            default:
                return 0;
        }
    }

    // A key the field takes (it edits there or types a character): its press goes to the field, its release nowhere.
    static boolean key(KeyEvent e) {
        int edit = editKey(e), c = edit != 0 ? 0 : e.getUnicodeChar();
        if (edit == 0 && (c <= 0 || (c & KeyCharacterMap.COMBINING_ACCENT) != 0 || Character.isISOControl(c)))
            return false;
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            if (edit != 0) nativeKey(edit);
            else type(new String(Character.toChars(c)));
        }
        return true;
    }

    // What the keyboard sends. The field's text is not mirrored here: words being composed are typed into the field as
    // they change (the old one deleted first), and committed text is typed at its cursor.
    static final class Connection extends BaseInputConnection {
        private int composing;  // code points of the composing text the field holds now

        Connection(View view) {
            super(view, false);
        }

        private void replaceComposing(CharSequence text) {
            for (; composing > 0; composing--) nativeKey(KeyEvent.KEYCODE_DEL);
            String s = text == null ? "" : text.toString();
            type(s);
            composing = s.codePointCount(0, s.length());
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            replaceComposing(text);
            return true;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            replaceComposing(text);
            composing = 0;
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing = 0;
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            // A deletion while a word is being composed: what's left of it can't be told apart any more, so the next
            // composing text is typed after it rather than replacing it (no committed letter deleted by mistake)
            composing = 0;
            for (int i = 0; i < beforeLength; i++) nativeKey(KeyEvent.KEYCODE_DEL);
            for (int i = 0; i < afterLength; i++) nativeKey(KeyEvent.KEYCODE_FORWARD_DEL);
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            composing = 0;
            nativeKey(KeyEvent.KEYCODE_ENTER);
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            if (event.getAction() == KeyEvent.ACTION_MULTIPLE && event.getKeyCode() == KeyEvent.KEYCODE_UNKNOWN) {
                if (event.getCharacters() != null) type(event.getCharacters());
                return true;
            }
            if (event.getKeyCode() == KeyEvent.KEYCODE_DEL || event.getKeyCode() == KeyEvent.KEYCODE_FORWARD_DEL)
                composing = 0;  // as in deleteSurroundingText
            return key(event) || super.sendKeyEvent(event);
        }
    }
}
