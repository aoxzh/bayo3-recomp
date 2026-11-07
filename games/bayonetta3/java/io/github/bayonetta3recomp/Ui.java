package io.github.bayonetta3recomp;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

// The palette and the few widgets the launcher is made of, in code (no XML resources), after the desktop launcher
// (tools/b3_launcher.py): art darkened towards BG, frosted panels, cyan for focus, gold for the primary action.
final class Ui {
    static final int BG = 0xff0b0a14, PANEL = 0xff13111f, FIELD = 0xff1d1a2e, HOVER = 0xff221e36, LINE = 0xff2e2944;
    static final int TEXT = 0xfff1ecf7, MUTED = 0xffa59fbd, FAINT = 0xff6d6787;
    static final int CYAN = 0xff5fe3ef, GOLD = 0xffe8b75c, GOLD_DIM = 0xff6e5631, RED = 0xffff5f7a, GREEN = 0xff79e0a6;

    private Ui() {}

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics()));
    }

    static int alpha(int color, int a) { return (color & 0x00ffffff) | (a << 24); }

    static GradientDrawable rect(Context c, int fill, int stroke, float strokeDp, float radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(c, radiusDp));
        if (strokeDp > 0) g.setStroke(dp(c, strokeDp), stroke);
        return g;
    }

    // Idle / selected / pressed / focused: focus is always the cyan ring, whatever else the control shows.
    static Drawable states(Context c, int fill, int stroke, int selectedFill, int selectedStroke, float radiusDp) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_focused, android.R.attr.state_selected},
                rect(c, selectedFill, CYAN, 2.5f, radiusDp));
        s.addState(new int[] {android.R.attr.state_focused}, rect(c, fill, CYAN, 2.5f, radiusDp));
        s.addState(new int[] {android.R.attr.state_pressed}, rect(c, HOVER, selectedStroke, 1.5f, radiusDp));
        s.addState(new int[] {android.R.attr.state_selected}, rect(c, selectedFill, selectedStroke, 1.5f, radiusDp));
        s.addState(new int[] {}, rect(c, fill, stroke, 1, radiusDp));
        return s;
    }

    static TextView text(Context c, CharSequence s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        if (bold) t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        return t;
    }

    static void focusable(View v) {
        v.setFocusable(true);
        v.setClickable(true);
        v.setDefaultFocusHighlightEnabled(false);
    }

    // A pill button: 48dp tall at least, with the focus ring.
    static TextView button(Context c, String label, boolean primary) {
        TextView b = text(c, label, primary ? 16 : 14, primary ? BG : TEXT, true);
        b.setGravity(Gravity.CENTER);
        b.setMinHeight(dp(c, 48));
        b.setMinWidth(dp(c, 48));
        b.setPadding(dp(c, 18), 0, dp(c, 18), 0);
        b.setBackground(primary ? states(c, GOLD, GOLD, GOLD, GOLD, 24)
                : states(c, alpha(PANEL, 0xd8), LINE, alpha(PANEL, 0xd8), LINE, 24));
        focusable(b);
        return b;
    }

    // A settings row: a label (and a hint under it) on the left, the value on the right. A choice row steps its value
    // with a tap, or with left/right on a D-pad or stick, so the whole panel works from a controller.
    static final class Row extends LinearLayout {
        interface Stepper { void step(int dir); }

        final TextView label, hint, value;
        Stepper stepper;

        Row(Context c, String labelText, String hintText) {
            super(c);
            setOrientation(HORIZONTAL);
            setGravity(Gravity.CENTER_VERTICAL);
            setMinimumHeight(dp(c, 56));
            setPadding(dp(c, 16), dp(c, 8), dp(c, 16), dp(c, 8));
            setBackground(states(c, alpha(FIELD, 0xb0), alpha(LINE, 0x00), alpha(FIELD, 0xb0), LINE, 12));
            focusable(this);
            LinearLayout left = new LinearLayout(c);
            left.setOrientation(VERTICAL);
            label = text(c, labelText, 15, TEXT, true);
            left.addView(label);
            hint = text(c, hintText == null ? "" : hintText, 12, MUTED, false);
            hint.setPadding(0, dp(c, 4), 0, 0);
            hint.setVisibility(hintText == null ? GONE : VISIBLE);
            left.addView(hint);
            addView(left, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
            value = text(c, "", 14, GOLD, true);
            value.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
            value.setPadding(dp(c, 12), 0, 0, 0);
            addView(value, new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT));
            setOnClickListener(v -> { if (stepper != null) stepper.step(1); });
        }

        void show(String v, boolean on) {
            value.setText(stepper != null && !v.equals("On") && !v.equals("Off") ? "‹  " + v + "  ›" : v);
            value.setTextColor(on ? GOLD : MUTED);
        }

        @Override public boolean onKeyDown(int code, KeyEvent e) {
            if (stepper != null && (code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT)) {
                stepper.step(code == KeyEvent.KEYCODE_DPAD_LEFT ? -1 : 1);
                return true;
            }
            return super.onKeyDown(code, e);
        }
    }
}
