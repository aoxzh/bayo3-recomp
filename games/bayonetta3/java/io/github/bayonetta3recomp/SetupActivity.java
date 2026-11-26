package io.github.bayonetta3recomp;

import android.app.Activity;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

// The on-device setup's screen (Setup.java): where the dump goes, one button, then the progress and the last lines
// of the log until the game is built. The screen stays on while it runs; leaving it does not stop the setup, and a
// setup that stopped (the app closed, the phone restarted) resumes from its last finished step when run again.
public final class SetupActivity extends Activity {
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView status, detail, log, action;
    private ProgressBar bar;
    private ScrollView logScroll;
    private Setup setup;

    private int dp(float v) { return Ui.dp(this, v); }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().setBackgroundDrawable(new ColorDrawable(Ui.BG));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(28), dp(20), dp(28), dp(20));
        c.addView(Ui.text(this, Tr.t("Set up the game on this phone"), 22, Ui.TEXT, true));
        detail = Ui.text(this, "", 13.5f, Ui.MUTED, false);
        detail.setLineSpacing(dp(3), 1);
        detail.setPadding(0, dp(10), 0, dp(12));
        c.addView(detail);
        status = Ui.text(this, "", 15, Ui.TEXT, true);
        c.addView(status);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-1, dp(10));
        blp.topMargin = dp(8);
        blp.bottomMargin = dp(8);
        c.addView(bar, blp);
        log = Ui.text(this, "", 11.5f, Ui.FAINT, false);
        log.setTypeface(Typeface.MONOSPACE);
        logScroll = new ScrollView(this);
        logScroll.setBackground(Ui.rect(this, Ui.alpha(Ui.PANEL, 0xe0), Ui.LINE, 1, 12));
        logScroll.setPadding(dp(12), dp(8), dp(12), dp(8));
        logScroll.addView(log);
        c.addView(logScroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        row.setPadding(0, dp(12), 0, 0);
        TextView back = Ui.button(this, Tr.t("Back"), false);
        back.setOnClickListener(v -> finish());
        row.addView(back);
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(12), 1));
        action = Ui.button(this, Tr.t("Start setup"), true);
        action.setOnClickListener(v -> act());
        row.addView(action);
        c.addView(row, new LinearLayout.LayoutParams(-1, -2));
        FrameLayout f = new FrameLayout(this);
        f.addView(c, new FrameLayout.LayoutParams(-1, -1));
        setContentView(f);
        action.requestFocus();
    }

    @Override protected void onResume() {
        super.onResume();
        setup = Setup.current();
        if (setup != null) setup.listen(() -> ui.post(this::show));
        show();
    }

    @Override protected void onPause() {
        super.onPause();
        if (setup != null) setup.listen(null);
    }

    private void act() {
        if (setup != null && setup.running) {
            setup.cancel();
            return;
        }
        if (setup != null && setup.ok) {
            finish();
            return;
        }
        String problem = Setup.dumpProblem(this);
        if (problem != null) {
            show();
            return;
        }
        if (android.os.Build.VERSION.SDK_INT >= 33
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS") != android.content.pm.PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, 1);  // the progress notification
        setup = Setup.start(this);
        setup.listen(() -> ui.post(this::show));
        show();
    }

    private void show() {
        String problem = Setup.dumpProblem(this);
        String where = Setup.where(this);
        detail.setText(Tr.f("Copy the exefs and romfs folders you extracted from your own Bayonetta 3 (update 1.2.0) "
                + "with your emulator to the phone: %1$s/dump, or anywhere in Download or Documents, on %2$s (the setup "
                + "finds them; Settings > Game folder picks Internal storage or an SD card). The setup moves them into "
                + "the game's folder on %2$s (no copy when they are on the same storage), translates the game's code "
                + "and shaders and compiles them here: about 30 to 60 minutes and 6 GB of free space in Internal "
                + "storage. Keep the phone charging. You can leave this screen: the setup goes on in the background, "
                + "its progress in the notification; if it stops, Start setup carries on where it left off. One small "
                + "file is downloaded (the Kraken decoder's source, which may not ship in the APK); nothing leaves the "
                + "phone.\n\nFaster: a computer that has set the game up builds it for the phone in a few minutes and "
                + "copies it over USB (the desktop launcher's Send to my phone).", DataCheck.TITLE, where));
        if (setup == null) {
            status.setText(problem == null ? Tr.f("Ready to start · game files found in %s", Setup.dumpFolder(this))
                    : Tr.f("Not yet: %s", problem));
            status.setTextColor(problem == null ? Ui.TEXT : Ui.RED);
            bar.setProgress(0);
            log.setText("");
            action.setText(Tr.t("Start setup"));
            action.setAlpha(problem == null ? 1f : 0.55f);
            return;
        }
        synchronized (setup) {
            StringBuilder b = new StringBuilder();
            int from = Math.max(0, setup.lines.size() - 60);
            for (int i = from; i < setup.lines.size(); i++) b.append(setup.lines.get(i)).append('\n');
            log.setText(b);
            if (setup.running) {
                String p = setup.phase;
                if (setup.total > 0) p += " · " + setup.done + " / " + setup.total;
                status.setText(p);
                status.setTextColor(Ui.TEXT);
                bar.setIndeterminate(setup.total <= 0);
                if (setup.total > 0) bar.setProgress((int) (1000L * setup.done / setup.total));
                action.setText(Tr.t("Stop"));
            } else if (setup.ok) {
                status.setText(Tr.t("Done: the game is ready"));
                status.setTextColor(Ui.GREEN);
                bar.setIndeterminate(false);
                bar.setProgress(1000);
                action.setText(Tr.t("Back to the launcher"));
            } else {
                status.setText(setup.error != null ? setup.error : Tr.t("Stopped"));
                status.setTextColor(Ui.RED);
                bar.setIndeterminate(false);
                action.setText(Tr.t("Start setup"));
            }
            action.setAlpha(1f);
        }
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_A && e.getAction() == KeyEvent.ACTION_UP) {
            View f = getCurrentFocus();
            (f != null ? f : action).performClick();
            return true;
        }
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_B && e.getAction() == KeyEvent.ACTION_UP) {
            finish();
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
}
