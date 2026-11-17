// The look of the app's own screens (Home, Prepare): a sheet of paper on the island's sky, the title, plain text and
// round buttons, and the language they speak (English, or Brazilian Portuguese on a phone set to Portuguese).
package io.github.acnhrecomp;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Locale;

final class Screen {
    static final int BROWN = Color.rgb(92, 70, 50), TEXT = Color.rgb(120, 100, 80), RED = Color.rgb(190, 80, 80),
                     GREEN_TEXT = Color.rgb(70, 140, 60), ORANGE = Color.rgb(200, 120, 40);
    static final int GREEN = Color.rgb(90, 170, 90), BLUE = Color.rgb(70, 140, 210), WHITE = Color.WHITE;

    private Screen() {}

    static boolean pt() { return Locale.getDefault().getLanguage().equals("pt"); }
    static String t(String en, String ptBr) { return pt() ? ptBr : en; }

    static int dp(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }

    static Button button(Context c, String text, int color, View.OnClickListener click) {
        Button b = new Button(c);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(17);
        b.setTextColor(color == WHITE ? BROWN : Color.WHITE);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(c, 26));
        b.setBackground(bg);
        b.setPadding(dp(c, 22), dp(c, 10), dp(c, 22), dp(c, 10));
        b.setOnClickListener(click);
        b.setMinHeight(dp(c, 54));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(c, 10);
        b.setLayoutParams(lp);
        return b;
    }

    static TextView text(Context c, String s, float size, int color) {
        TextView v = new TextView(c);
        v.setText(s);
        v.setTextSize(size);
        v.setTextColor(color);
        v.setPadding(0, dp(c, 6), 0, dp(c, 6));
        return v;
    }

    static TextView heading(Context c, String s) {
        TextView v = text(c, s, 18, BROWN);
        v.setTypeface(Typeface.DEFAULT_BOLD);
        v.setPadding(0, dp(c, 14), 0, dp(c, 2));
        return v;
    }

    // The sheet the screen's views go into, set as the activity's content (scrolling when taller than the screen).
    static LinearLayout page(Activity a, String title) {
        ScrollView scroll = new ScrollView(a);
        scroll.setFillViewport(true);
        GradientDrawable sky = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] {Color.rgb(160, 222, 200), Color.rgb(196, 230, 160)});
        scroll.setBackground(sky);
        LinearLayout outer = new LinearLayout(a);
        outer.setGravity(Gravity.CENTER);
        outer.setPadding(dp(a, 24), dp(a, 24), dp(a, 24), dp(a, 24));
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(a, 28), dp(a, 24), dp(a, 28), dp(a, 24));
        GradientDrawable paper = new GradientDrawable();
        paper.setColor(Color.rgb(255, 250, 232));
        paper.setCornerRadius(dp(a, 28));
        box.setBackground(paper);
        outer.addView(box, new LinearLayout.LayoutParams(Math.min(dp(a, 560), a.getResources().getDisplayMetrics().widthPixels - dp(a, 48)),
                LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(outer);
        TextView t = new TextView(a);
        t.setText(title);
        t.setTextSize(26);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextColor(BROWN);
        box.addView(t);
        a.setContentView(scroll);
        return box;
    }
}
