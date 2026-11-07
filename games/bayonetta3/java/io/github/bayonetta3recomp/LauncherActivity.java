package io.github.bayonetta3recomp;

import android.app.Activity;
import android.app.GameManager;
import android.app.NativeActivity;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.RenderEffect;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

// The home screen in front of the game: checks that the data is in place (and asks for all-files access to read it),
// picks a performance profile and a few settings, writes them to <data root>/config.txt in the runtime's KEY=VALUE
// format, and starts android.app.NativeActivity (libbayonetta3.so). Landscape, dark, over the launcher art; every
// control is at least 48dp and reachable with a D-pad or stick (A selects, B backs out, Start starts, L1/R1 profile).
public final class LauncherActivity extends Activity {
    private static final String[][] LANGUAGES = {
            {"", "Default"}, {"en-US", "English (US)"}, {"en-GB", "English (UK)"}, {"ja", "日本語"},
            {"fr", "Français"}, {"fr-CA", "Français (Canada)"}, {"de", "Deutsch"}, {"it", "Italiano"},
            {"es", "Español"}, {"es-419", "Español (Latinoamérica)"}, {"nl", "Nederlands"},
            {"pt", "Português"}, {"pt-BR", "Português (Brasil)"}, {"ru", "Русский"},
            {"ko", "한국어"}, {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}};
    private static final String[][] TOUCH = {{"1", "Hide with a pad"}, {"2", "Always"}, {"0", "Off"}};
    private static final int[] OPACITY = {40, 55, 70, 85, 100};
    private static final int[] SIZE = {70, 85, 100, 115, 130};
    private static final String[][] PROFILES = {
            {Config.SAVER, "Battery saver", "30 fps · coolest, longest on battery"},
            {Config.BALANCED, "Balanced 40", "40 fps · steady on recent chips"},
            {Config.UNLOCKED, "Unlocked", "Up to 60 fps · falls as the phone warms"}};

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Config cfg;
    private DataCheck data;
    private String gpu;
    private int thermal = -1;

    private FrameLayout root;
    private ImageView art;
    private View home, firstRun;
    private FrameLayout panelLayer;
    private View panel;
    private TextView start, hints;
    private final LinearLayout[] profileRows = new LinearLayout[PROFILES.length];
    private TextView dataValue, cacheValue, padValue, chipValue, heatValue, note;
    private View dataDot, cacheDot, padDot, heatDot;
    private Ui.Row langRow, swapRow, muteRow, touchRow, opacityRow, sizeRow;
    private LinearLayout advanced;
    private TextView advancedRow, folderText;
    private EditText advancedText;
    private InputManager.InputDeviceListener padListener;
    private PowerManager.OnThermalStatusChangedListener thermalListener;

    // ---- lifecycle ------------------------------------------------------------------------------------------------

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().setBackgroundDrawable(new ColorDrawable(Ui.BG));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28)
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        build();
        setContentView(root);
        hideBars();
        loadArt();
        new Thread(() -> {
            String g = null;
            try {
                g = ChipInfo.gpu();
            } catch (Throwable ignored) {
            }
            final String found = g;
            ui.post(() -> { gpu = found; showChip(); });
        }, "gpu-name").start();

        InputManager im = getSystemService(InputManager.class);
        padListener = new InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) { showPads(); }
            @Override public void onInputDeviceRemoved(int id) { showPads(); }
            @Override public void onInputDeviceChanged(int id) { showPads(); }
        };
        im.registerInputDeviceListener(padListener, ui);
        PowerManager pm = getSystemService(PowerManager.class);
        thermalListener = s -> { thermal = s; showHeat(); };
        pm.addThermalStatusListener(getMainExecutor(), thermalListener);
        thermal = pm.getCurrentThermalStatus();
    }

    @Override protected void onResume() {
        super.onResume();
        refresh();
    }

    @Override protected void onPause() {
        super.onPause();
        if (cfg != null && panelLayer.getVisibility() == View.VISIBLE) saveAdvanced();
    }

    @Override protected void onDestroy() {
        getSystemService(InputManager.class).unregisterInputDeviceListener(padListener);
        getSystemService(PowerManager.class).removeThermalStatusListener(thermalListener);
        super.onDestroy();
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideBars();
    }

    private void hideBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    private boolean allFilesAccess() { return Environment.isExternalStorageManager(); }

    // Re-read everything: the permission (it may have just been granted in Settings), the data folder, config.txt.
    private void refresh() {
        boolean blocked = !allFilesAccess() && !DataCheck.hasData(DataCheck.ownRoot(this));
        firstRun.setVisibility(blocked ? View.VISIBLE : View.GONE);
        home.setVisibility(blocked ? View.GONE : View.VISIBLE);
        if (blocked) {
            firstRun.findViewWithTag("grant").requestFocus();
            return;
        }
        data = DataCheck.run(this);
        cfg = Config.load(new File(data.root, "config.txt"));
        showData();
        showProfile();
        showSettings();
        showPads();
        showHeat();
        showChip();
        showNote();
        if (getCurrentFocus() == null || !getCurrentFocus().isShown()) start.requestFocus();
    }

    // ---- layout ---------------------------------------------------------------------------------------------------

    private int dp(float v) { return Ui.dp(this, v); }

    private void build() {
        root = new FrameLayout(this);
        art = new ImageView(this);
        art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        root.addView(art, match());
        // The art darkened towards BG: strongly behind the text on the left, lightly over the cathedral on the right,
        // and again along the bottom where Start sits.
        View scrim = new View(this);
        scrim.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {Ui.alpha(Ui.BG, 0xf2), Ui.alpha(Ui.BG, 0xc0), Ui.alpha(Ui.BG, 0x50)}));
        root.addView(scrim, match());
        View bottom = new View(this);
        bottom.setBackground(new GradientDrawable(GradientDrawable.Orientation.BOTTOM_TOP,
                new int[] {Ui.alpha(Ui.BG, 0xd0), Ui.alpha(Ui.BG, 0x00)}));
        root.addView(bottom, new FrameLayout.LayoutParams(-1, dp(180), Gravity.BOTTOM));

        FrameLayout safe = new FrameLayout(this);  // the display cutout and bars kept clear
        root.addView(safe, match());
        safe.setOnApplyWindowInsetsListener((v, insets) -> {
            int l = 0, t = 0, r = 0, b = 0;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets i = insets.getInsetsIgnoringVisibility(
                        WindowInsets.Type.displayCutout() | WindowInsets.Type.navigationBars());
                l = i.left; t = i.top; r = i.right; b = i.bottom;
            }
            int m = dp(20);
            v.setPadding(Math.max(m, l + dp(8)), Math.max(dp(16), t), Math.max(m, r + dp(8)), Math.max(dp(16), b));
            return insets;
        });
        home = buildHome();
        safe.addView(home, match());
        firstRun = buildFirstRun();
        safe.addView(firstRun, match());

        panelLayer = new FrameLayout(this);
        panelLayer.setVisibility(View.GONE);
        View dim = new View(this);
        dim.setBackgroundColor(Ui.alpha(Ui.BG, 0xa0));
        dim.setOnClickListener(v -> closePanel());
        panelLayer.addView(dim, match());
        panel = buildPanel();
        panelLayer.addView(panel, new FrameLayout.LayoutParams(dp(460), -1, Gravity.END));
        root.addView(panelLayer, match());
    }

    private static FrameLayout.LayoutParams match() { return new FrameLayout.LayoutParams(-1, -1); }

    private View buildHome() {
        LinearLayout h = new LinearLayout(this);
        h.setOrientation(LinearLayout.HORIZONTAL);

        // Left: the title and the status card.
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        TextView title = Ui.text(this, "BAYONETTA 3", 34, Ui.TEXT, false);
        title.setTypeface(Typeface.create("serif", Typeface.BOLD));
        title.setLetterSpacing(0.08f);
        title.setShadowLayer(dp(10), 0, 0, Ui.alpha(Ui.CYAN, 0x60));
        left.addView(title);
        TextView sub = Ui.text(this, "Static recompilation · Android", 13, Ui.MUTED, false);
        sub.setLetterSpacing(0.04f);
        sub.setPadding(0, dp(6), 0, dp(14));
        left.addView(sub);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(10), dp(16), dp(10));
        card.setBackground(Ui.rect(this, Ui.alpha(Ui.PANEL, 0xc8), Ui.alpha(Ui.LINE, 0xc0), 1, 16));
        dataDot = new View(this);
        dataValue = statusRow(card, "Game data", dataDot);
        cacheDot = new View(this);
        cacheValue = statusRow(card, "Shader cache", cacheDot);
        padDot = new View(this);
        padValue = statusRow(card, "Controller", padDot);
        chipValue = statusRow(card, "Chip", null);
        heatDot = new View(this);
        heatValue = statusRow(card, "Temperature", heatDot);
        left.addView(card, new LinearLayout.LayoutParams(-1, -2));
        note = Ui.text(this, "", 11.5f, Ui.FAINT, false);
        note.setLineSpacing(dp(2), 1);
        note.setPadding(dp(4), dp(10), 0, 0);
        left.addView(note);
        View fill = new View(this);
        left.addView(fill, new LinearLayout.LayoutParams(-1, 0, 1));
        hints = Ui.text(this, "Ⓐ Select    Ⓑ Back    ☰ Start    L1 / R1  Profile    Ⓨ Settings", 12,
                Ui.MUTED, false);
        hints.setVisibility(View.GONE);
        left.addView(hints);
        ScrollView ls = new ScrollView(this);
        ls.setVerticalScrollBarEnabled(false);
        ls.setFillViewport(true);
        ls.addView(left);
        h.addView(ls, new LinearLayout.LayoutParams(0, -1, 1));

        View gap = new View(this);
        h.addView(gap, new LinearLayout.LayoutParams(dp(28), 1));

        // Right: settings and report at the top, the profiles and Start under the right thumb.
        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.END);
        TextView report = Ui.button(this, "Report a bug", false);
        report.setOnClickListener(v -> shareReport());
        top.addView(report);
        View sp = new View(this);
        top.addView(sp, new LinearLayout.LayoutParams(dp(10), 1));
        TextView settings = Ui.button(this, "Settings", false);
        settings.setOnClickListener(v -> openPanel());
        top.addView(settings);
        right.addView(top, new LinearLayout.LayoutParams(-1, -2));
        right.addView(new View(this), new LinearLayout.LayoutParams(-1, 0, 1));

        TextView perf = Ui.text(this, "PERFORMANCE", 11.5f, Ui.MUTED, true);
        perf.setLetterSpacing(0.16f);
        perf.setPadding(dp(4), 0, 0, dp(8));
        right.addView(perf);
        for (int i = 0; i < PROFILES.length; i++) {
            final String id = PROFILES[i][0];
            LinearLayout r = new LinearLayout(this);
            r.setOrientation(LinearLayout.VERTICAL);
            r.setGravity(Gravity.CENTER_VERTICAL);
            r.setMinimumHeight(dp(52));
            r.setPadding(dp(16), dp(6), dp(16), dp(6));
            r.setBackground(Ui.states(this, Ui.alpha(Ui.PANEL, 0xc8), Ui.alpha(Ui.LINE, 0xc0),
                    Ui.alpha(Ui.GOLD_DIM, 0x90), Ui.GOLD, 14));
            Ui.focusable(r);
            r.addView(Ui.text(this, PROFILES[i][1], 15, Ui.TEXT, true));
            TextView d = Ui.text(this, PROFILES[i][2], 12, Ui.MUTED, false);
            d.setPadding(0, dp(3), 0, 0);
            r.addView(d);
            r.setOnClickListener(v -> setProfile(id));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.bottomMargin = dp(8);
            right.addView(r, lp);
            profileRows[i] = r;
        }
        start = Ui.button(this, "▶   START", true);
        start.setTextSize(TypedValue.COMPLEX_UNIT_SP, 21);
        start.setLetterSpacing(0.14f);
        start.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        start.setBackground(Ui.states(this, Ui.GOLD, Ui.GOLD, Ui.GOLD, Ui.GOLD, 20));
        start.setElevation(dp(6));
        start.setOnClickListener(v -> startGame());
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(-1, dp(68));
        slp.topMargin = dp(4);
        right.addView(start, slp);
        h.addView(right, new LinearLayout.LayoutParams(dp(300), -1));
        return h;
    }

    private TextView statusRow(LinearLayout card, String label, View dot) {
        LinearLayout r = new LinearLayout(this);
        r.setGravity(Gravity.TOP);
        r.setPadding(0, dp(5), 0, dp(5));
        FrameLayout dotBox = new FrameLayout(this);
        if (dot != null) {
            dot.setBackground(Ui.rect(this, Ui.FAINT, 0, 0, 4));
            dotBox.addView(dot, new FrameLayout.LayoutParams(dp(8), dp(8), Gravity.CENTER));
        }
        r.addView(dotBox, new LinearLayout.LayoutParams(dp(18), dp(16)));
        TextView l = Ui.text(this, label, 13, Ui.MUTED, false);
        r.addView(l, new LinearLayout.LayoutParams(dp(104), -2));
        TextView v = Ui.text(this, "…", 14, Ui.TEXT, false);
        v.setLineSpacing(dp(3), 1);
        r.addView(v, new LinearLayout.LayoutParams(0, -2, 1));
        card.addView(r);
        return v;
    }

    private View buildFirstRun() {
        FrameLayout f = new FrameLayout(this);
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(28), dp(24), dp(28), dp(24));
        c.setBackground(Ui.rect(this, Ui.alpha(Ui.PANEL, 0xe8), Ui.LINE, 1, 20));
        TextView t = Ui.text(this, "Allow access to the game folder", 22, Ui.TEXT, true);
        c.addView(t);
        TextView b = Ui.text(this, "The game runs from Internal storage/" + DataCheck.TITLE + ": the files you made from "
                + "your own copy of the game. Android lets an app read a folder there only with “All files "
                + "access”.\n\nThe app uses it for that folder alone: the game data, your saves, the shader cache "
                + "and the log. Turn it on for this app on the next page, then come back.", 14, Ui.MUTED, false);
        b.setLineSpacing(dp(3), 1);
        b.setPadding(0, dp(12), 0, dp(20));
        c.addView(b);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        TextView again = Ui.button(this, "Check again", false);
        again.setOnClickListener(v -> refresh());
        row.addView(again);
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(12), 1));
        TextView grant = Ui.button(this, "Allow all files access", true);
        grant.setTag("grant");
        grant.setOnClickListener(v -> requestAccess());
        row.addView(grant);
        c.addView(row, new LinearLayout.LayoutParams(-1, -2));
        ScrollView s = new ScrollView(this);
        s.addView(c);
        f.addView(s, new FrameLayout.LayoutParams(dp(560), -2, Gravity.CENTER));
        return f;
    }

    private View buildPanel() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackground(new ColorDrawable(Ui.alpha(Ui.PANEL, 0xfa)));
        p.setClickable(true);
        p.setPadding(dp(20), dp(16), dp(20), dp(12));
        p.setOnApplyWindowInsetsListener((v, insets) -> {
            int r = 0;
            if (Build.VERSION.SDK_INT >= 30)
                r = insets.getInsetsIgnoringVisibility(WindowInsets.Type.displayCutout()
                        | WindowInsets.Type.navigationBars()).right;
            v.setPadding(dp(20), dp(16), dp(20) + r, dp(12));
            return insets;
        });
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = Ui.text(this, "Settings", 22, Ui.TEXT, true);
        head.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        TextView done = Ui.button(this, "Done", false);
        done.setOnClickListener(v -> closePanel());
        head.addView(done);
        p.addView(head);

        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(0, dp(12), 0, dp(12));
        langRow = row(list, "Language", "The game's text and voices");
        langRow.stepper = d -> {
            String cur = cfg.get("language", "");
            int i = 0;
            for (int k = 0; k < LANGUAGES.length; k++) if (LANGUAGES[k][0].equalsIgnoreCase(cur)) i = k;
            i = (i + d + LANGUAGES.length) % LANGUAGES.length;
            cfg.set("language", LANGUAGES[i][0].isEmpty() ? null : LANGUAGES[i][0]);
            saveAndShow();
        };
        swapRow = row(list, "Xbox button layout", "On: the button marked A is the game's A. Off: by position, as on Switch");
        swapRow.stepper = d -> { cfg.set("NX_PAD_SWAP", on("NX_PAD_SWAP") ? null : "1"); saveAndShow(); };
        muteRow = row(list, "Mute", "No sound at all");
        muteRow.stepper = d -> { cfg.set("NX_MUTE", on("NX_MUTE") ? null : "1"); saveAndShow(); };
        section(list, "TOUCH CONTROLS");
        touchRow = row(list, "Touch controls", "Drawn over the game");
        touchRow.stepper = d -> {
            int i = index(TOUCH, cfg.get("NX_TOUCH_SHOW", "1"));
            i = (i + d + TOUCH.length) % TOUCH.length;
            cfg.set("NX_TOUCH_SHOW", TOUCH[i][0]);
            saveAndShow();
        };
        opacityRow = row(list, "Opacity", null);
        opacityRow.stepper = d -> { cfg.set("NX_TOUCH_OPACITY", "" + stepInt(OPACITY, "NX_TOUCH_OPACITY", 85, d)); saveAndShow(); };
        sizeRow = row(list, "Size", null);
        sizeRow.stepper = d -> { cfg.set("NX_TOUCH_SIZE", "" + stepInt(SIZE, "NX_TOUCH_SIZE", 100, d)); saveAndShow(); };

        section(list, "SUPPORT");
        Ui.Row share = row(list, "Share a bug report", "A zip of nx.log, config.txt and the chip info, through the share sheet");
        share.value.setText("›");
        share.setOnClickListener(v -> shareReport());

        advancedRow = Ui.text(this, "Advanced  ▸", 14, Ui.MUTED, true);
        advancedRow.setGravity(Gravity.CENTER_VERTICAL);
        advancedRow.setMinHeight(dp(48));
        advancedRow.setPadding(dp(16), 0, dp(16), 0);
        advancedRow.setBackground(Ui.states(this, 0, 0, 0, 0, 12));
        Ui.focusable(advancedRow);
        advancedRow.setOnClickListener(v -> {
            boolean show = advanced.getVisibility() != View.VISIBLE;
            if (!show) saveAdvanced();
            advanced.setVisibility(show ? View.VISIBLE : View.GONE);
            advancedRow.setText(show ? "Advanced  ▾" : "Advanced  ▸");
        });
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(-1, -2);
        alp.topMargin = dp(10);
        list.addView(advancedRow, alp);
        advanced = new LinearLayout(this);
        advanced.setOrientation(LinearLayout.VERTICAL);
        advanced.setVisibility(View.GONE);
        folderText = Ui.text(this, "", 12, Ui.MUTED, false);
        folderText.setPadding(dp(4), dp(6), dp(4), dp(8));
        folderText.setLineSpacing(dp(2), 1);
        advanced.addView(folderText);
        advancedText = new EditText(this);
        advancedText.setTypeface(Typeface.MONOSPACE);
        advancedText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        advancedText.setTextColor(Ui.TEXT);
        advancedText.setHintTextColor(Ui.FAINT);
        advancedText.setHint("NX_KEY=value, one per line");
        advancedText.setGravity(Gravity.TOP | Gravity.START);
        advancedText.setMinLines(6);
        advancedText.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        advancedText.setHorizontallyScrolling(true);
        advancedText.setPadding(dp(12), dp(10), dp(12), dp(10));
        advancedText.setBackground(Ui.states(this, Ui.FIELD, Ui.LINE, Ui.FIELD, Ui.LINE, 10));
        advancedText.setDefaultFocusHighlightEnabled(false);
        advanced.addView(advancedText, new LinearLayout.LayoutParams(-1, -2));
        list.addView(advanced, new LinearLayout.LayoutParams(-1, -2));

        ScrollView s = new ScrollView(this);
        s.setVerticalScrollBarEnabled(false);
        s.addView(list);
        p.addView(s, new LinearLayout.LayoutParams(-1, 0, 1));
        return p;
    }

    private Ui.Row row(LinearLayout list, String label, String hint) {
        Ui.Row r = new Ui.Row(this, label, hint);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = dp(6);
        list.addView(r, lp);
        return r;
    }

    private void section(LinearLayout list, String name) {
        TextView t = Ui.text(this, name, 11.5f, Ui.MUTED, true);
        t.setLetterSpacing(0.16f);
        t.setPadding(dp(4), dp(14), 0, dp(8));
        list.addView(t);
    }

    // ---- showing the state ----------------------------------------------------------------------------------------

    private void dot(View d, int color) { if (d != null) d.setBackground(Ui.rect(this, color, 0, 0, 4)); }

    private void showData() {
        StringBuilder b = new StringBuilder();
        if (!data.folderExists) {
            b.append(data.rootLabel()).append(" not found.\nCreate it and copy the game data into it: analysis/, "
                    + "extract/romfs/ and spirv/.");
        } else if (data.ready()) {
            b.append("Ready · ").append(data.rootLabel());
        } else {
            b.append("Missing in ").append(data.rootLabel()).append(':');
            for (DataCheck.Item i : data.missing) b.append("\n• ").append(i.path).append(" — ").append(i.why);
        }
        dataValue.setText(b);
        dataValue.setTextColor(data.ready() ? Ui.TEXT : Ui.RED);
        dot(dataDot, data.ready() ? Ui.GREEN : Ui.RED);
        start.setAlpha(data.ready() ? 1f : 0.55f);
        if (data.cacheFiles > 0) {
            cacheValue.setText(DataCheck.size(data.cacheBytes) + " · warm, fewer stutters");
            dot(cacheDot, Ui.GREEN);
        } else {
            cacheValue.setText("Empty · first scenes stutter while it fills");
            dot(cacheDot, Ui.GOLD);
        }
    }

    private void setProfile(String id) {
        cfg.setProfile(id);
        save();
        showProfile();
    }

    private void showProfile() {
        String cur = cfg.profile();
        for (int i = 0; i < PROFILES.length; i++) profileRows[i].setSelected(PROFILES[i][0].equals(cur));
    }

    private boolean on(String key) { return cfg.get(key, "").length() > 0 && !cfg.get(key, "").equals("0"); }

    private static int index(String[][] table, String v) {
        for (int i = 0; i < table.length; i++) if (table[i][0].equalsIgnoreCase(v)) return i;
        return 0;
    }

    private int stepInt(int[] steps, String key, int def, int dir) {
        int cur;
        try {
            cur = Integer.parseInt(cfg.get(key, "" + def));
        } catch (NumberFormatException e) {
            cur = def;
        }
        int i = 0;
        for (int k = 0; k < steps.length; k++) if (Math.abs(steps[k] - cur) < Math.abs(steps[i] - cur)) i = k;
        return steps[Math.max(0, Math.min(steps.length - 1, i + dir))];
    }

    private void showSettings() {
        String lang = cfg.get("language", "");
        langRow.show(LANGUAGES[index(LANGUAGES, lang)][1], true);
        swapRow.show(on("NX_PAD_SWAP") ? "On" : "Off", on("NX_PAD_SWAP"));
        muteRow.show(on("NX_MUTE") ? "On" : "Off", on("NX_MUTE"));
        String touch = cfg.get("NX_TOUCH_SHOW", "1");
        touchRow.show(TOUCH[index(TOUCH, touch)][1], !touch.equals("0"));
        boolean touchOn = !touch.equals("0");
        opacityRow.show(cfg.get("NX_TOUCH_OPACITY", "85") + "%", touchOn);
        sizeRow.show(cfg.get("NX_TOUCH_SIZE", "100") + "%", touchOn);
        folderText.setText("Data folder: " + data.root.getAbsolutePath() + "\nOther lines of config.txt, kept as "
                + "they are. A line for a setting above is replaced by that setting.");
        advancedText.setText(cfg.otherText());
    }

    private void showPads() {
        if (padValue == null) return;
        List<String> names = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            // A controller number is what the system gives real pads (built-in ones too); vendor virtual devices that
            // report joystick classes (force-feedback nodes and the like) have none.
            if (d == null || d.isVirtual() || d.getControllerNumber() <= 0) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                if (!names.contains(d.getName())) names.add(d.getName());
        }
        if (names.isEmpty()) {
            boolean touch = cfg == null || !"0".equals(cfg.get("NX_TOUCH_SHOW", "1"));
            padValue.setText(touch ? "None · touch controls" : "None · touch controls are off in Settings");
            dot(padDot, touch ? Ui.FAINT : Ui.GOLD);
        } else {
            padValue.setText(String.join(", ", names));
            dot(padDot, Ui.GREEN);
        }
        hints.setVisibility(names.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void showChip() {
        if (chipValue == null) return;
        chipValue.setText(ChipInfo.soc() + (gpu != null ? " · " + gpu : ""));
    }

    private static final String[] HEAT = {"Cool", "Light · fine", "Warm · clocks may drop",
            "Hot · expect a lower frame rate", "Critical · let it cool", "Emergency", "Shutting down"};

    private void showHeat() {
        if (heatValue == null) return;
        int s = Math.max(0, Math.min(HEAT.length - 1, thermal));
        heatValue.setText(thermal < 0 ? "Unknown" : HEAT[s]);
        dot(heatDot, s <= 1 ? Ui.GREEN : s == 2 ? Ui.GOLD : Ui.RED);
    }

    private String gameMode() {
        if (Build.VERSION.SDK_INT < 31) return null;
        GameManager gm = getSystemService(GameManager.class);
        if (gm == null) return null;
        switch (gm.getGameMode()) {
            case GameManager.GAME_MODE_STANDARD: return "Standard";
            case GameManager.GAME_MODE_PERFORMANCE: return "Performance";
            case GameManager.GAME_MODE_BATTERY: return "Battery";
            case 4: return "Custom";
            default: return null;
        }
    }

    private void showNote() {
        String mode = gameMode();
        note.setText((mode != null ? "Game mode: " + mode + ". " : "")
                + "Some phones hold CPU clocks low for apps outside the system's game list: if the frame rate stays "
                + "low while cool, add this app there.");
    }

    // ---- actions --------------------------------------------------------------------------------------------------

    private void saveAndShow() {
        save();
        showSettings();
        showPads();
    }

    private boolean save() {
        try {
            cfg.save();
            return true;
        } catch (IOException e) {
            Toast.makeText(this, "Could not write config.txt: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return false;
        }
    }

    private void saveAdvanced() {
        String t = advancedText.getText().toString();
        if (t.equals(cfg.otherText())) return;
        cfg.setOtherText(t);
        save();
        advancedText.setText(cfg.otherText());
    }

    private void requestAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void startGame() {
        if (cfg == null) return;
        if (!data.ready()) {
            Toast.makeText(this, "The game data is incomplete: the list on the left says what is missing.",
                    Toast.LENGTH_LONG).show();
            return;
        }
        if (panelLayer.getVisibility() == View.VISIBLE) saveAdvanced();
        if (!cfg.values.containsKey("NX_PROFILE")) cfg.setProfile(cfg.profile());
        if (!save()) return;
        startActivity(new Intent(this, NativeActivity.class));
        finish();
    }

    private void openPanel() {
        if (cfg == null) return;
        panelLayer.setVisibility(View.VISIBLE);
        panel.setTranslationX(dp(460));
        panel.animate().translationX(0).setDuration(180).start();
        if (Build.VERSION.SDK_INT >= 31)
            art.setRenderEffect(RenderEffect.createBlurEffect(dp(14), dp(14), Shader.TileMode.CLAMP));
        langRow.requestFocus();
    }

    private void closePanel() {
        if (panelLayer.getVisibility() != View.VISIBLE) return;
        saveAdvanced();
        hideKeyboard();
        panel.animate().translationX(dp(460)).setDuration(150).withEndAction(() -> panelLayer.setVisibility(View.GONE)).start();
        if (Build.VERSION.SDK_INT >= 31) art.setRenderEffect(null);
        start.requestFocus();
    }

    private void hideKeyboard() {
        android.view.inputmethod.InputMethodManager imm = getSystemService(android.view.inputmethod.InputMethodManager.class);
        if (imm != null) imm.hideSoftInputFromWindow(root.getWindowToken(), 0);
    }

    @Override public void onBackPressed() {
        if (panelLayer.getVisibility() == View.VISIBLE) closePanel();
        else super.onBackPressed();
    }

    // A: select the focused control (the first press only shows where focus is). B: back. Start: play. Y: settings.
    // L1 / R1: the previous / next profile. The stick moves focus like the D-pad (the framework synthesises the keys).
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        boolean up = e.getAction() == KeyEvent.ACTION_UP;
        switch (k) {
            case KeyEvent.KEYCODE_BUTTON_A: {
                if (!up) return true;
                View f = getCurrentFocus();
                if (f == null || root.isInTouchMode()) {
                    View target = firstRun.getVisibility() == View.VISIBLE ? firstRun.findViewWithTag("grant") : start;
                    if (f == null || !f.isShown()) target.requestFocusFromTouch();
                    else f.requestFocusFromTouch();
                } else if (f instanceof EditText) {
                    getSystemService(android.view.inputmethod.InputMethodManager.class).showSoftInput(f, 0);
                } else {
                    f.performClick();
                }
                return true;
            }
            case KeyEvent.KEYCODE_BUTTON_B:
                if (up) {
                    if (panelLayer.getVisibility() == View.VISIBLE) closePanel();
                    else moveTaskToBack(true);
                }
                return true;
            case KeyEvent.KEYCODE_BUTTON_START:
                if (up && home.getVisibility() == View.VISIBLE) startGame();
                return true;
            case KeyEvent.KEYCODE_BUTTON_Y:
                if (up && home.getVisibility() == View.VISIBLE) {
                    if (panelLayer.getVisibility() == View.VISIBLE) closePanel();
                    else openPanel();
                }
                return true;
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_R1:
                if (up && cfg != null && home.getVisibility() == View.VISIBLE) {
                    int i = 0;
                    for (int n = 0; n < PROFILES.length; n++) if (PROFILES[n][0].equals(cfg.profile())) i = n;
                    i = Math.max(0, Math.min(PROFILES.length - 1, i + (k == KeyEvent.KEYCODE_BUTTON_L1 ? -1 : 1)));
                    setProfile(PROFILES[i][0]);
                }
                return true;
            default:
                return super.dispatchKeyEvent(e);
        }
    }

    // ---- art and the support report -------------------------------------------------------------------------------

    private void loadArt() {
        new Thread(() -> {
            Bitmap b = null;
            try (InputStream in = getAssets().open("launcher/background.jpg")) {
                b = BitmapFactory.decodeStream(in);
            } catch (IOException ignored) {
            }
            final Bitmap bm = b;
            if (bm != null) ui.post(() -> art.setImageBitmap(bm));
        }, "art").start();
    }

    private String report() {
        StringBuilder b = new StringBuilder();
        String version = "?";
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = pi.versionName + " (" + pi.getLongVersionCode() + ")";
        } catch (Exception ignored) {
        }
        b.append("Bayonetta 3 Recomp: launcher report, ")
                .append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date())).append('\n');
        b.append("app: ").append(version).append('\n');
        b.append("android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        b.append("soc: ").append(ChipInfo.soc()).append('\n');
        b.append("gpu: ").append(gpu == null ? "?" : gpu).append('\n');
        b.append("abi: ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
        b.append("cpus: ").append(Runtime.getRuntime().availableProcessors()).append('\n');
        b.append("thermal status: ").append(thermal).append('\n');
        String mode = gameMode();
        b.append("game mode: ").append(mode == null ? "unsupported" : mode).append('\n');
        b.append("all-files access: ").append(allFilesAccess()).append('\n');
        if (data != null) {
            b.append("data folder: ").append(data.root).append(data.ready() ? " (ready)" : " (incomplete)").append('\n');
            for (DataCheck.Item i : data.missing) b.append("  missing: ").append(i.path).append('\n');
            b.append("pipeline cache: ").append(data.cacheFiles).append(" file(s), ")
                    .append(DataCheck.size(data.cacheBytes)).append('\n');
        }
        if (cfg != null) b.append("profile: ").append(cfg.profile()).append('\n');
        b.append("controllers: ").append(padValue.getText()).append('\n');
        return b.toString();
    }

    private void shareReport() {
        final String text = report();
        final File root = data != null ? data.root : null;
        Toast.makeText(this, "Preparing the report…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            File dir = SupportProvider.dir(this);
            File[] old = dir.listFiles();
            if (old != null) for (File f : old) f.delete();
            dir.mkdirs();
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(new Date());
            File zip = new File(dir, "bayonetta3-report-" + stamp + ".zip");
            String error = null;
            try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(zip))) {
                z.putNextEntry(new ZipEntry("chip-and-app.txt"));
                z.write(text.getBytes("UTF-8"));
                z.closeEntry();
                if (root != null) {
                    addTail(z, new File(root, "nx.log"), 24L << 20);
                    addTail(z, new File(root, "config.txt"), 1L << 20);
                }
            } catch (IOException e) {
                error = e.getMessage();
            }
            final String err = error;
            ui.post(() -> {
                if (err != null) {
                    Toast.makeText(this, "Could not make the report: " + err, Toast.LENGTH_LONG).show();
                    return;
                }
                Uri uri = SupportProvider.uri(zip);
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType("application/zip");
                send.putExtra(Intent.EXTRA_STREAM, uri);
                send.putExtra(Intent.EXTRA_SUBJECT, "Bayonetta 3 Recomp bug report");
                send.putExtra(Intent.EXTRA_TEXT, "What happened, and what you were doing:\n\n");
                send.setClipData(ClipData.newRawUri(zip.getName(), uri));
                send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                startActivity(Intent.createChooser(send, "Send the bug report"));
            });
        }, "report").start();
    }

    // The last `max` bytes of a file (a long nx.log keeps its end, where a crash is).
    private static void addTail(ZipOutputStream z, File f, long max) throws IOException {
        if (!f.isFile()) return;
        z.putNextEntry(new ZipEntry(f.getName()));
        try (FileInputStream in = new FileInputStream(f)) {
            long skip = Math.max(0, f.length() - max);
            while (skip > 0) {
                long s = in.skip(skip);
                if (s <= 0) break;
                skip -= s;
            }
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) z.write(buf, 0, n);
        }
        z.closeEntry();
    }
}
