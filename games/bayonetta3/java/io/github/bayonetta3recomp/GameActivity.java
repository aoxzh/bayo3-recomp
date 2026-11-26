package io.github.bayonetta3recomp;

import android.app.AlertDialog;
import android.app.Dialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.util.List;

// The game: a NativeActivity whose code is libbayonetta3.so (android_main in runtime/src/platform_android.cpp, loaded
// by libb3loader.so), in its own process (:game), so Quit to launcher can end the game and leave the launcher running.
// It adds the in-game menu, opened with the controller's Guide/Home button or Back three times in a row (the runtime
// sees them in platform_android_input.cpp, holds the game at its present as the PC's settings panel does, and calls
// openMenu): Resume, the frame rate profile, the touch controls, Report a bug and Quit to launcher. Touch and a
// controller both work in it (the D-pad or stick moves, A selects, B resumes).
public class GameActivity extends NativeActivity {
    private static final String TAG = "b3menu";
    // {value, shown}: as the launcher's (LauncherActivity PROFILES and TOUCH).
    private static final String[][] PROFILES = {
            {Config.SAVER, Tr.t("Battery saver 30")}, {Config.BALANCED, Tr.t("Balanced 40")},
            {Config.UNLOCKED, Tr.t("Unlocked 60")}};
    private static final String[][] TOUCH = {{"1", Tr.t("Hide with a pad")}, {"2", Tr.t("Always")}, {"0", Tr.t("Off")}};

    // Registered by the runtime (platform_android.cpp ANativeActivity_onCreate); a library without them throws.
    private static native void nativeMenu(boolean open);
    private static native void nativeTouch(int show);

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Dialog menu;
    private boolean quitting;
    private String startProfile;  // the profile the game started with: a different one waits for the next start

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Config c = config();
        startProfile = c == null ? null : c.profile();
    }

    @Override protected void onDestroy() {
        if (menu != null) {
            quitting = true;  // no resume on the way out
            menu.dismiss();
            menu = null;
        }
        super.onDestroy();
    }

    // From the runtime's input thread, the game already held.
    @SuppressWarnings("unused")
    void openMenu() {
        ui.post(() -> {
            try {
                if (menu == null && !isFinishing()) showMenu();
            } catch (Throwable t) {
                Log.e(TAG, "menu: " + t);
                menu = null;
                resume();
            }
        });
    }

    private static void resume() {
        try {
            nativeMenu(false);
        } catch (Throwable t) {
            Log.e(TAG, "nativeMenu: " + t);
        }
    }

    private File root() { return DataCheck.chooseRoot(this); }

    private Config config() {
        File r = root();
        return r == null ? null : Config.load(new File(r, "config.txt"));
    }

    private int dp(float v) { return Ui.dp(this, v); }

    private static int index(String[][] list, String value) {
        for (int i = 0; i < list.length; i++) if (list[i][0].equals(value)) return i;
        return 0;
    }

    private void showMenu() {
        final Config cfg = config();
        final Dialog d = new Dialog(this, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        menu = d;

        FrameLayout dim = new FrameLayout(this);
        dim.setBackgroundColor(Ui.alpha(Ui.BG, 0xb8));
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(22), dp(18), dp(22), dp(18));
        panel.setBackground(Ui.rect(this, Ui.alpha(Ui.PANEL, 0xf0), Ui.LINE, 1, 18));

        TextView title = Ui.text(this, Tr.t("Paused"), 22, Ui.TEXT, true);
        panel.addView(title);
        TextView keys = Ui.text(this, Tr.t("Ⓐ Select    Ⓑ Resume    ‹ › Change"), 12, Ui.MUTED, false);
        keys.setPadding(0, dp(6), 0, dp(14));
        panel.addView(keys);

        TextView resume = Ui.button(this, Tr.t("Resume"), true);
        resume.setOnClickListener(v -> d.dismiss());
        panel.addView(resume, new LinearLayout.LayoutParams(-1, -2));
        gap(panel, 12);

        final Ui.Row fps = new Ui.Row(this, Tr.t("Frame rate"), null);
        final int[] pi = {index(PROFILES, cfg == null ? Config.SAVER : cfg.profile())};
        fps.stepper = dir -> {
            if (cfg == null) return;
            pi[0] = (pi[0] + dir + PROFILES.length) % PROFILES.length;
            cfg.setProfile(PROFILES[pi[0]][0]);
            save(cfg);
            showProfile(fps, PROFILES[pi[0]]);
        };
        showProfile(fps, PROFILES[pi[0]]);
        panel.addView(fps, new LinearLayout.LayoutParams(-1, -2));
        gap(panel, 8);

        final Ui.Row touch = new Ui.Row(this, Tr.t("Touch controls"), null);
        final int[] ti = {index(TOUCH, cfg == null ? "1" : cfg.get("NX_TOUCH_SHOW", "1"))};
        touch.stepper = dir -> {
            ti[0] = (ti[0] + dir + TOUCH.length) % TOUCH.length;
            if (cfg != null) {
                cfg.set("NX_TOUCH_SHOW", TOUCH[ti[0]][0]);
                save(cfg);
            }
            try {
                nativeTouch(Integer.parseInt(TOUCH[ti[0]][0]));  // at once
            } catch (Throwable t) {
                Log.e(TAG, "nativeTouch: " + t);
            }
            touch.show(TOUCH[ti[0]][1], true);
        };
        touch.show(TOUCH[ti[0]][1], true);
        panel.addView(touch, new LinearLayout.LayoutParams(-1, -2));
        gap(panel, 12);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        TextView report = Ui.button(this, Tr.t("Report a bug"), false);
        report.setOnClickListener(v -> report());
        buttons.addView(report, new LinearLayout.LayoutParams(0, -2, 1));
        View g = new View(this);
        buttons.addView(g, new LinearLayout.LayoutParams(dp(10), 1));
        TextView quit = Ui.button(this, Tr.t("Quit to launcher"), false);
        quit.setOnClickListener(v -> confirmQuit());
        buttons.addView(quit, new LinearLayout.LayoutParams(0, -2, 1));
        panel.addView(buttons, new LinearLayout.LayoutParams(-1, -2));

        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(panel);
        dim.addView(scroll, new FrameLayout.LayoutParams(Math.min(dp(480), getResources().getDisplayMetrics().widthPixels
                - dp(32)), -2, Gravity.CENTER));
        d.setContentView(dim);

        d.setOnKeyListener((dlg, code, e) -> {
            boolean up = e.getAction() == KeyEvent.ACTION_UP;
            switch (code) {
                case KeyEvent.KEYCODE_BUTTON_A: {
                    if (!up) return true;
                    View f = d.getCurrentFocus();
                    if (f == null || !f.isShown()) resume.requestFocus();
                    else f.performClick();
                    return true;
                }
                case KeyEvent.KEYCODE_BUTTON_B:
                case KeyEvent.KEYCODE_BUTTON_MODE:
                case KeyEvent.KEYCODE_BUTTON_START:
                case KeyEvent.KEYCODE_BACK:
                    if (up) d.dismiss();
                    return true;
                default:
                    return false;
            }
        });
        d.setOnDismissListener(x -> {
            menu = null;
            if (!quitting) resume();
        });

        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(0));
            if (Build.VERSION.SDK_INT >= 28)
                w.getAttributes().layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            hideBars(w);
        }
        d.show();
        resume.requestFocus();
    }

    private void gap(LinearLayout l, float h) {
        View v = new View(this);
        l.addView(v, new LinearLayout.LayoutParams(1, dp(h)));
    }

    private void showProfile(Ui.Row row, String[] p) {
        boolean later = startProfile != null && !startProfile.equals(p[0]);
        row.show(p[1], true);
        row.hint.setText(Tr.t("Restart to apply: quit to the launcher and start again"));
        row.hint.setVisibility(later ? View.VISIBLE : View.GONE);
    }

    private void save(Config cfg) {
        try {
            cfg.save();
        } catch (IOException e) {
            android.widget.Toast.makeText(this, Tr.f("Not saved: %s", e.getMessage()), android.widget.Toast.LENGTH_LONG).show();
        }
    }

    private void report() {
        File r = root();
        DataCheck data = DataCheck.run(this);
        Config cfg = config();
        List<String> names = Report.pads();
        String pads = names.isEmpty() ? "None · touch controls" : String.join(", ", names);  // the report: English
        String gpu = Report.keptGpu(this);
        Report.make(this, Report.text(this, "in-game", gpu, data, cfg, pads), r, true, gpu);
    }

    private void confirmQuit() {
        AlertDialog a = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(Tr.t("Quit to the launcher?"))
                .setMessage(Tr.t("The game closes. Progress since your last save is lost."))
                .setPositiveButton(Tr.t("Quit"), (x, y) -> quit())
                .setNegativeButton(Tr.t("Cancel"), null)
                .create();
        a.setOnShowListener(x -> a.getButton(AlertDialog.BUTTON_NEGATIVE).requestFocus());  // the safe choice first
        a.show();
    }

    // The launcher in a fresh task (the main process, which stays), this one cleared: its destroy ends the game's
    // process (platform_android.cpp: destroyRequested -> _exit). The game stays held until then.
    private void quit() {
        quitting = true;
        Log.i(TAG, "quit to the launcher");
        startActivity(new Intent(this, LauncherActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        ui.postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 4000);  // if it lingers
    }

    private static void hideBars(Window w) {
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(false);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }
}
