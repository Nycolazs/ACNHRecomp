// Nextendo's sign-in page inside the app (nextendo.cpp sign_in_start, started by platform_android.cpp open_sign_in).
// In the phone's browser the game went to the background while the player signed in, and Android froze it (the
// cached apps freezer) or ended it for memory, so the page's last step, the redirect to the game's listener on
// 127.0.0.1, hung or found nobody there (Discord, 0.9.0). Here the app stays in front: the page loads in a WebView,
// its redirect loads here too (res/xml/network_security_config.xml allows plain HTTP to 127.0.0.1 and nowhere else),
// the game's listener answers it, and the screen closes. Cancel, or Back on the first page, ends the sign-in as a
// refusal does: the redirect address with error=access_denied, which the listener takes as "cancelled".
package io.github.acnhrecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.net.HttpURLConnection;
import java.net.URL;

public class SignIn extends Activity {
    private WebView web;
    private String redirect;  // the authorize URL's redirect_uri: http://127.0.0.1:<port>/callback
    private String state;
    private boolean done;     // the redirect was reached, or the sign-in ended: nothing more to send the listener

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        String url = getIntent().getStringExtra("url");
        if (url == null) {
            done = true;
            finish();
            return;
        }
        Uri page = Uri.parse(url);
        redirect = page.getQueryParameter("redirect_uri");
        state = page.getQueryParameter("state");
        if (redirect == null || !redirect.startsWith("http://127.0.0.1:")) redirect = null;  // not one to finish here
        try {
            if (redirect != null) web = new WebView(this);
        } catch (RuntimeException e) {  // no WebView on this phone (turned off, or being updated)
            web = null;
        }
        if (web == null) {  // the browser, as before
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, page));
                done = true;
            } catch (RuntimeException e) {
                tellCancelled();
            }
            finish();
            return;
        }
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);  // the page and its captcha are scripts
        s.setDomStorageEnabled(true);  // it keeps the login in localStorage
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setGeolocationEnabled(false);
        s.setSupportMultipleWindows(false);  // "Create an account" (window.open) opens here; Back comes back
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                if (!r.isForMainFrame()) return false;
                String scheme = r.getUrl().getScheme();
                if (!"http".equals(scheme) && !"https".equals(scheme)) return true;  // intent:, mailto:...: not from here
                if (isRedirect(r.getUrl().toString())) done = true;
                return false;
            }

            @Override
            public void onPageStarted(WebView v, String u, Bitmap icon) {
                if (isRedirect(u)) done = true;
            }

            @Override
            public void onPageFinished(WebView v, String u) {
                if (isRedirect(u)) finish();  // the game has its answer (or its listener had already given up)
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest r, WebResourceError e) {
                if (r.isForMainFrame() && isRedirect(r.getUrl().toString())) finish();
            }

            // The page's process ended (it crashed, or Android needed the memory). Unhandled, Android would end the
            // whole app with it, the game included: the screen closes instead, and Sign in can be pressed again.
            @Override
            public boolean onRenderProcessGone(WebView v, RenderProcessGoneDetail d) {
                if (v.getParent() instanceof ViewGroup) ((ViewGroup) v.getParent()).removeView(v);
                v.destroy();
                if (v == web) web = null;
                cancel();
                return true;
            }
        });

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(24, 24, 30));
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(16), dp(4), dp(8), dp(4));
        TextView title = new TextView(this);
        title.setText("Nextendo");
        title.setTextColor(Color.WHITE);
        title.setTextSize(18);
        bar.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button close = new Button(this);
        close.setText(android.R.string.cancel);
        close.setAllCaps(false);
        close.setOnClickListener(v -> cancel());
        bar.addView(close);
        root.addView(bar);
        final ProgressBar progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        root.addView(progress, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(3)));
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onProgressChanged(WebView v, int p) {
                progress.setProgress(p);
                progress.setVisibility(p >= 100 ? View.INVISIBLE : View.VISIBLE);
            }
        });
        root.addView(web, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        // Android 15 draws apps under the system bars and the keyboard: the screen keeps clear of them. Before Android
        // 11 the window is laid out around them (and resized for the keyboard, adjustResize in the manifest).
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            root.setOnApplyWindowInsetsListener((v, in) -> {
                Insets i = in.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout()
                                        | WindowInsets.Type.ime());
                v.setPadding(i.left, i.top, i.right, i.bottom);
                return WindowInsets.CONSUMED;
            });
        }
        setContentView(root);
        web.loadUrl(url);
    }

    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) web.goBack();
        else cancel();
    }

    @Override
    protected void onDestroy() {
        if (isFinishing()) tellCancelled();  // closed some other way: the game stops waiting too
        if (web != null) web.destroy();
        super.onDestroy();
    }

    private void cancel() {
        tellCancelled();
        finish();
    }

    // What the page sends when the permissions are refused (RFC 6749 4.1.2.1), so the listener ends now rather than
    // after its timeout, and Sign in can be pressed again at once.
    private void tellCancelled() {
        if (done || redirect == null) return;
        done = true;
        final String to = redirect + "?error=access_denied" + (state != null ? "&state=" + Uri.encode(state) : "");
        new Thread(() -> {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(to).openConnection();
                c.setConnectTimeout(3000);
                c.setReadTimeout(3000);
                c.getResponseCode();
                c.disconnect();
            } catch (Exception ignored) {
                // the listener had already stopped
            }
        }).start();
    }

    private boolean isRedirect(String u) {
        return u != null && redirect != null && (u.equals(redirect) || u.startsWith(redirect + "?") || u.startsWith(redirect + "#"));
    }

    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}
