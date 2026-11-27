package io.github.bayonetta3recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.GameManager;
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
            {"", Tr.t("System language")}, {"en-US", "English (US)"}, {"en-GB", "English (UK)"}, {"ja", "日本語"},
            {"fr", "Français"}, {"fr-CA", "Français (Canada)"}, {"de", "Deutsch"}, {"it", "Italiano"},
            {"es", "Español"}, {"es-419", "Español (Latinoamérica)"}, {"nl", "Nederlands"},
            {"pt", "Português"}, {"pt-BR", "Português (Brasil)"}, {"ru", "Русский"},
            {"ko", "한국어"}, {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}};
    // NX_MENU_COMBO: a controller pair that opens the in-game menu, besides Guide and Back x3 (#12).
    private static final String[][] MENU = {{"", Tr.t("Guide or Back x3")}, {"select_start", Tr.t("Select + Start")},
            {"l3_r3", Tr.t("L3 + R3")}};
    // NX_HANDHELD (platform_android.cpp: unset means handheld on a phone): handheld renders 1280x720; docked is the
    // TV mode, up to 1600x900 shown at 1080p, much heavier on the GPU.
    private static final String[][] RESOLUTION = {{"", Tr.t("Handheld · 720p")}, {"0", Tr.t("Docked · 1080p")}};
    private static final String[][] TOUCH = {{"1", Tr.t("Hide with a pad")}, {"2", Tr.t("Always")}, {"0", Tr.t("Off")}};
    private static final int[] OPACITY = {40, 55, 70, 85, 100};
    private static final int[] SIZE = {70, 85, 100, 115, 130};
    private static final String[][] PROFILES = {
            {Config.SAVER, Tr.t("Battery saver"), Tr.t("30 fps · coolest, longest on battery")},
            {Config.BALANCED, Tr.t("Balanced 40"), Tr.t("40 fps · steady on recent chips")},
            {Config.UNLOCKED, Tr.t("Unlocked"), Tr.t("Up to 60 fps · falls as the phone warms")}};

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Config cfg;
    private DataCheck data;
    private GameImport gameImport;
    private String gpu;
    private DeviceCheck.Result spec;  // the phone against the minimum (DeviceCheck.java), once the GPU name is known
    private boolean specShown;
    private int thermal = -1;

    private FrameLayout root;
    private ImageView art;
    private View home, firstRun;
    private FrameLayout panelLayer;
    private View panel;
    private TextView start, hints;
    private final LinearLayout[] profileRows = new LinearLayout[PROFILES.length];
    private TextView dataValue, cacheValue, padValue, chipValue, heatValue, note;
    private LinearLayout gameModeCard;
    private LinearLayout updateCard;  // a newer release on GitHub (Updates.check), hidden until one is found
    private TextView updateTitle;
    private String updateUrl = Updates.RELEASES;
    private Updates.Release update;   // the newer release found at start
    private TextView updateHow, updateNow;
    private boolean downloading;
    private View dataDot, cacheDot, padDot, heatDot;
    private Ui.Row langRow, resRow, swapRow, menuRow, updatesRow, muteRow, touchRow, opacityRow, sizeRow, folderRow;
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
        Updates.check(this, r -> {
            update = r;
            updateUrl = r.page;
            showUpdate();
        });
        new Thread(() -> {
            String g = null;
            try {
                g = ChipInfo.gpu();
            } catch (Throwable ignored) {
            }
            final String found = g;
            DeviceCheck.Result r = null;
            try {
                r = DeviceCheck.run(this, found, false);
            } catch (Throwable ignored) {
            }
            final DeviceCheck.Result checked = r;
            ui.post(() -> {
                gpu = found;
                Report.keepGpu(this, found);  // for the in-game report (GameActivity)
                spec = checked;
                showChip();
                // Below the minimum: say so once when the launcher opens, before anyone starts a setup.
                if (spec != null && !spec.ok() && !tried(spec) && !specShown) {
                    specShown = true;
                    specDialog(spec, null);
                }
            });
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
        if (gameImport == null) {
            gameImport = new GameImport(this, what -> ui.post(() -> {
                refresh();
                Toast.makeText(this, "The game built on your computer is installed.", Toast.LENGTH_LONG).show();
            }));
        }
        gameImport.start();  // the computer's b3_win.py phone sends the game it built (GameImport.java)
    }

    @Override protected void onPause() {
        super.onPause();
        if (gameImport != null) gameImport.stop();
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
        showUpdate();
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
        LinearLayout titleRow = new LinearLayout(this);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        ImageView icon = new ImageView(this);  // the app icon (games/bayonetta3/res)
        icon.setImageResource(getApplicationInfo().icon);
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(dp(48), dp(48));
        ilp.rightMargin = dp(12);
        titleRow.addView(icon, ilp);
        TextView title = Ui.text(this, "BAYONETTA 3", 34, Ui.TEXT, false);
        title.setTypeface(Typeface.create("serif", Typeface.BOLD));
        title.setLetterSpacing(0.08f);
        title.setShadowLayer(dp(10), 0, 0, Ui.alpha(Ui.CYAN, 0x60));
        titleRow.addView(title);
        left.addView(titleRow);
        TextView sub = Ui.text(this, Tr.t("Static recompilation · Android"), 13, Ui.MUTED, false);
        sub.setLetterSpacing(0.04f);
        sub.setPadding(0, dp(6), 0, dp(14));
        left.addView(sub);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(10), dp(16), dp(10));
        card.setBackground(Ui.rect(this, Ui.alpha(Ui.PANEL, 0xc8), Ui.alpha(Ui.LINE, 0xc0), 1, 16));
        dataDot = new View(this);
        dataValue = statusRow(card, Tr.t("Game data"), dataDot);
        cacheDot = new View(this);
        cacheValue = statusRow(card, Tr.t("Shader cache"), cacheDot);
        padDot = new View(this);
        padValue = statusRow(card, Tr.t("Controller"), padDot);
        chipValue = statusRow(card, Tr.t("Chip"), null);
        heatDot = new View(this);
        heatValue = statusRow(card, Tr.t("Temperature"), heatDot);
        left.addView(card, new LinearLayout.LayoutParams(-1, -2));
        left.addView(buildGameModeCard(), gameModeParams());
        left.addView(buildUpdateCard(), gameModeParams());
        View fill = new View(this);
        left.addView(fill, new LinearLayout.LayoutParams(-1, 0, 1));
        hints = Ui.text(this, Tr.t("Ⓐ Select    Ⓑ Back    ☰ Start    L1 / R1  Profile    Ⓨ Settings"), 12,
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
        TextView follow = Ui.button(this, Tr.t("Follow @aoxzh"), false);
        follow.setOnClickListener(v -> Updates.open(this, Updates.AUTHOR));
        top.addView(follow);
        top.addView(new View(this), new LinearLayout.LayoutParams(dp(10), 1));
        TextView report = Ui.button(this, Tr.t("Report a bug"), false);
        report.setOnClickListener(v -> reportIssue());
        top.addView(report);
        View sp = new View(this);
        top.addView(sp, new LinearLayout.LayoutParams(dp(10), 1));
        TextView settings = Ui.button(this, Tr.t("Settings"), false);
        settings.setOnClickListener(v -> openPanel());
        top.addView(settings);
        right.addView(top, new LinearLayout.LayoutParams(-1, -2));
        right.addView(new View(this), new LinearLayout.LayoutParams(-1, 0, 1));

        TextView perf = Ui.text(this, Tr.t("PERFORMANCE"), 11.5f, Ui.MUTED, true);
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
        start = Ui.button(this, Tr.t("▶   START"), true);
        start.setTextSize(TypedValue.COMPLEX_UNIT_SP, 21);
        start.setLetterSpacing(0.14f);
        start.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        start.setBackground(Ui.states(this, Ui.GOLD, Ui.GOLD, Ui.GOLD, Ui.GOLD, 20));
        start.setElevation(dp(6));
        start.setOnClickListener(v -> startGame());
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(-1, dp(68));
        slp.topMargin = dp(4);
        right.addView(start, slp);
        TextView menuHint = Ui.text(this, Tr.t("In the game: press Back 3 times (or Guide on a controller) for the menu"),
                11.5f, Ui.MUTED, false);
        menuHint.setGravity(Gravity.CENTER);
        menuHint.setPadding(0, dp(8), 0, 0);
        right.addView(menuHint, new LinearLayout.LayoutParams(-1, -2));
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
        TextView t = Ui.text(this, Tr.t("Allow access to the game folder"), 22, Ui.TEXT, true);
        c.addView(t);
        TextView b = Ui.text(this, Tr.f("The game runs from Internal storage/%s: the files you made from your own copy of the "
                + "game. Android lets an app read a folder there only with “All files access”.\n\nThe app uses it for "
                + "that folder alone: the game data, your saves, the shader cache and the log. Turn it on for this app "
                + "on the next page, then come back.", DataCheck.TITLE), 14, Ui.MUTED, false);
        b.setLineSpacing(dp(3), 1);
        b.setPadding(0, dp(12), 0, dp(20));
        c.addView(b);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        TextView again = Ui.button(this, Tr.t("Check again"), false);
        again.setOnClickListener(v -> refresh());
        row.addView(again);
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(12), 1));
        TextView grant = Ui.button(this, Tr.t("Allow all files access"), true);
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
        TextView title = Ui.text(this, Tr.t("Settings"), 22, Ui.TEXT, true);
        head.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        TextView done = Ui.button(this, Tr.t("Done"), false);
        done.setOnClickListener(v -> closePanel());
        head.addView(done);
        p.addView(head);

        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(0, dp(12), 0, dp(12));
        langRow = row(list, Tr.t("Language"), Tr.t("The game's text and voices"));
        langRow.stepper = d -> {
            String cur = cfg.get("language", "");
            int i = 0;
            for (int k = 0; k < LANGUAGES.length; k++) if (LANGUAGES[k][0].equalsIgnoreCase(cur)) i = k;
            i = (i + d + LANGUAGES.length) % LANGUAGES.length;
            cfg.set("language", LANGUAGES[i][0].isEmpty() ? null : LANGUAGES[i][0]);
            saveAndShow();
        };
        resRow = row(list, Tr.t("Resolution"), Tr.t("Docked draws the game as on a TV (up to 1600×900, shown at "
                + "1080p): sharper, but much heavier on the GPU, the battery and the heat. Takes effect at the next start"));
        resRow.stepper = d -> {
            int i = (index(RESOLUTION, cfg.get("NX_HANDHELD", "")) + d + RESOLUTION.length) % RESOLUTION.length;
            cfg.set("NX_HANDHELD", RESOLUTION[i][0].isEmpty() ? null : RESOLUTION[i][0]);
            saveAndShow();
        };
        swapRow = row(list, Tr.t("Xbox button layout"), Tr.t("On: the button marked A is the game's A. Off: by position, as on Switch"));
        swapRow.stepper = d -> { cfg.set("NX_PAD_SWAP", on("NX_PAD_SWAP") ? null : "1"); saveAndShow(); };
        menuRow = row(list, Tr.t("Menu button"), Tr.t("Opens the in-game menu. Guide and Back x3 always work too"));
        menuRow.stepper = d -> {
            int i = (index(MENU, cfg.get("NX_MENU_COMBO", "")) + d + MENU.length) % MENU.length;
            cfg.set("NX_MENU_COMBO", MENU[i][0].isEmpty() ? null : MENU[i][0]);
            saveAndShow();
        };
        muteRow = row(list, Tr.t("Mute"), Tr.t("No sound at all"));
        muteRow.stepper = d -> { cfg.set("NX_MUTE", on("NX_MUTE") ? null : "1"); saveAndShow(); };
        section(list, Tr.t("TOUCH CONTROLS"));
        touchRow = row(list, Tr.t("Touch controls"), Tr.t("Drawn over the game"));
        touchRow.stepper = d -> {
            int i = index(TOUCH, cfg.get("NX_TOUCH_SHOW", "1"));
            i = (i + d + TOUCH.length) % TOUCH.length;
            cfg.set("NX_TOUCH_SHOW", TOUCH[i][0]);
            saveAndShow();
        };
        opacityRow = row(list, Tr.t("Opacity"), null);
        opacityRow.stepper = d -> { cfg.set("NX_TOUCH_OPACITY", "" + stepInt(OPACITY, "NX_TOUCH_OPACITY", 85, d)); saveAndShow(); };
        sizeRow = row(list, Tr.t("Size"), null);
        sizeRow.stepper = d -> { cfg.set("NX_TOUCH_SIZE", "" + stepInt(SIZE, "NX_TOUCH_SIZE", 100, d)); saveAndShow(); };

        section(list, Tr.t("STORAGE"));
        folderRow = row(list, Tr.t("Game folder"), Tr.t("Where the game data, your saves and the shader cache are"));
        folderRow.stepper = d -> pickFolder();

        section(list, Tr.t("SUPPORT"));
        Ui.Row share = row(list, Tr.t("Share a bug report"), Tr.t("A zip of nx.log, config.txt and the chip info, through the share sheet"));
        share.value.setText("›");
        share.setOnClickListener(v -> shareReport());
        updatesRow = row(list, Tr.t("Check for updates"), Tr.t("When the launcher opens, ask GitHub whether a newer "
                + "version is out. Nothing is downloaded by itself"));
        updatesRow.stepper = d -> { Updates.setEnabled(this, !Updates.enabled(this)); showSettings(); };
        Ui.Row followRow = row(list, Tr.t("Follow the author"), Tr.t("@aoxzh on X: news, previews and releases"));
        followRow.value.setText("›");
        followRow.setOnClickListener(v -> Updates.open(this, Updates.AUTHOR));

        advancedRow = Ui.text(this, Tr.t("Advanced") + "  ▸", 14, Ui.MUTED, true);
        advancedRow.setGravity(Gravity.CENTER_VERTICAL);
        advancedRow.setMinHeight(dp(48));
        advancedRow.setPadding(dp(16), 0, dp(16), 0);
        advancedRow.setBackground(Ui.states(this, 0, 0, 0, 0, 12));
        Ui.focusable(advancedRow);
        advancedRow.setOnClickListener(v -> {
            boolean show = advanced.getVisibility() != View.VISIBLE;
            if (!show) saveAdvanced();
            advanced.setVisibility(show ? View.VISIBLE : View.GONE);
            advancedRow.setText(Tr.t("Advanced") + (show ? "  ▾" : "  ▸"));
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
        advancedText.setHint(Tr.t("NX_KEY=value, one per line"));
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
        if (needsSetup()) {  // an APK that builds the game here (Setup.java)
            dataValue.setText(Setup.current() != null && Setup.current().running
                    ? Tr.f("Setting up · %s (tap SET UP to see it)", Setup.current().phase)
                    : Tr.t("Not set up yet · SET UP builds the game from your dump here (30-60 min), or send it from a "
                    + "computer over USB in minutes (desktop launcher: Send to my phone)"));
            dataValue.setTextColor(Ui.GOLD);
            dot(dataDot, Ui.GOLD);
            start.setText(Tr.t("SET UP"));
            start.setAlpha(1f);
            cacheValue.setText(Tr.t("None yet"));
            dot(cacheDot, Ui.FAINT);
            return;
        }
        start.setText(Tr.t("▶   START"));
        if (!data.folderExists) {
            b.append(Tr.f("%s not found.\nCreate it and copy the game data into it: analysis/, extract/romfs/ and "
                    + "spirv/.", data.rootLabel()));
        } else if (data.ready()) {
            b.append(Tr.f("Ready · %s", data.rootLabel()));
        } else {
            b.append(Tr.f("Missing in %s:", data.rootLabel()));
            for (DataCheck.Item i : data.missing) b.append("\n• ").append(i.path).append(" — ").append(Tr.t(i.why));
        }
        dataValue.setText(b);
        dataValue.setTextColor(data.ready() ? Ui.TEXT : Ui.RED);
        dot(dataDot, data.ready() ? Ui.GREEN : Ui.RED);
        start.setAlpha(data.ready() ? 1f : 0.55f);
        if (data.cacheFiles > 0) {
            cacheValue.setText(Tr.f("%s · warm, fewer stutters", DataCheck.size(data.cacheBytes)));
            dot(cacheDot, Ui.GREEN);
        } else {
            cacheValue.setText(Tr.t("Empty · first scenes stutter while it fills"));
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
        swapRow.show(Tr.t(on("NX_PAD_SWAP") ? "On" : "Off"), on("NX_PAD_SWAP"));
        updatesRow.show(Tr.t(Updates.enabled(this) ? "On" : "Off"), Updates.enabled(this));
        String res = cfg.get("NX_HANDHELD", "");
        if (!res.equals("0")) res = "";  // NX_HANDHELD=1 written by hand: handheld, as unset
        resRow.show(RESOLUTION[index(RESOLUTION, res)][1], res.equals("0"));
        String menu = cfg.get("NX_MENU_COMBO", "");
        menuRow.show(MENU[index(MENU, menu)][1], !menu.isEmpty());
        muteRow.show(Tr.t(on("NX_MUTE") ? "On" : "Off"), on("NX_MUTE"));
        String touch = cfg.get("NX_TOUCH_SHOW", "1");
        touchRow.show(TOUCH[index(TOUCH, touch)][1], !touch.equals("0"));
        boolean touchOn = !touch.equals("0");
        opacityRow.show(cfg.get("NX_TOUCH_OPACITY", "85") + "%", touchOn);
        sizeRow.show(cfg.get("NX_TOUCH_SIZE", "100") + "%", touchOn);
        List<Storage.Volume> vols = Storage.volumes(this);
        Storage.Volume here = data.root.equals(DataCheck.ownRoot(this)) ? null : Storage.volumeOf(this, data.root);
        folderRow.show(here != null ? here.name : Tr.t("App folder"), true);
        folderRow.hint.setText(vols.size() > 1
                ? Tr.f("Where the game data, your saves and the shader cache are: Internal storage or %s", vols.get(1).name)
                : Tr.t("Where the game data, your saves and the shader cache are. An SD card shows up here once inserted"));
        folderText.setText(Tr.f("Data folder: %s\nOther lines of config.txt, kept as they are. A line for a setting "
                + "above is replaced by that setting.", data.root.getAbsolutePath()));
        advancedText.setText(cfg.otherText());
    }

    private void showPads() {
        if (padValue == null) return;
        List<String> names = Report.pads();
        if (names.isEmpty()) {
            boolean touch = cfg == null || !"0".equals(cfg.get("NX_TOUCH_SHOW", "1"));
            padValue.setText(Tr.t(touch ? "None · touch controls" : "None · touch controls are off in Settings"));
            dot(padDot, touch ? Ui.FAINT : Ui.GOLD);
        } else {
            padValue.setText(String.join(", ", names));
            dot(padDot, Ui.GREEN);
        }
        hints.setVisibility(names.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void showChip() {
        if (chipValue == null) return;
        String s = ChipInfo.soc() + (gpu != null ? " · " + gpu : "");
        if (spec != null && !spec.ok()) {
            chipValue.setText(s + "\n" + Tr.f("Below the minimum: %s", String.join("; ", spec.below)));
            chipValue.setTextColor(Ui.RED);
        } else if (spec != null && !spec.weak.isEmpty()) {
            chipValue.setText(s + "\n" + Tr.f("May struggle: %s", String.join("; ", spec.weak)));
            chipValue.setTextColor(Ui.GOLD);
        } else {
            chipValue.setText(s);
            chipValue.setTextColor(Ui.TEXT);
        }
    }

    // "Try anyway" is remembered for the same findings: a new reason (less free space) asks again.
    private boolean tried(DeviceCheck.Result r) {
        return r.key().equals(getSharedPreferences("device_check", MODE_PRIVATE).getString("tried", null));
    }

    private void specDialog(DeviceCheck.Result r, Runnable then) {
        new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle(Tr.t("This phone is below the minimum"))
                .setMessage(DeviceCheck.message(r))
                .setPositiveButton(Tr.t("Close"), null)
                .setNeutralButton(Tr.t("Try anyway"), (d, w) -> {
                    getSharedPreferences("device_check", MODE_PRIVATE).edit().putString("tried", r.key()).apply();
                    if (then != null) then.run();
                })
                .show();
    }

    private static final String[] HEAT = {Tr.t("Cool"), Tr.t("Light · fine"), Tr.t("Warm · clocks may drop"),
            Tr.t("Hot · expect a lower frame rate"), Tr.t("Critical · let it cool"), Tr.t("Emergency"),
            Tr.t("Shutting down")};

    private void showHeat() {
        if (heatValue == null) return;
        int s = Math.max(0, Math.min(HEAT.length - 1, thermal));
        heatValue.setText(thermal < 0 ? Tr.t("Unknown") : HEAT[s]);
        dot(heatDot, s <= 1 ? Ui.GREEN : s == 2 ? Ui.GOLD : Ui.RED);
    }

    private String gameMode() { return Report.gameMode(this); }


    // The game-mode card: many phones keep an app they do not know as a game at low clocks, and the game then cannot
    // run at full speed however cool the phone is. Always on the home screen until the player says it is done.
    private View buildGameModeCard() {
        gameModeCard = new LinearLayout(this);
        gameModeCard.setOrientation(LinearLayout.VERTICAL);
        gameModeCard.setPadding(dp(16), dp(12), dp(16), dp(12));
        gameModeCard.setBackground(Ui.rect(this, Ui.alpha(Ui.GOLD_DIM, 0x70), Ui.GOLD, 2, 16));
        TextView title = Ui.text(this, "⚡  " + Tr.t("Add this app to your phone's game mode first"), 15, Ui.GOLD, true);
        gameModeCard.addView(title);
        note = Ui.text(this, "", 12.5f, Ui.TEXT, false);
        note.setLineSpacing(dp(2), 1);
        note.setPadding(0, dp(6), 0, dp(8));
        gameModeCard.addView(note);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        TextView done = Ui.button(this, Tr.t("Done, hide this"), false);
        done.setOnClickListener(v -> {
            getSharedPreferences("launcher", MODE_PRIVATE).edit().putBoolean("game_mode_done", true).apply();
            gameModeCard.setVisibility(View.GONE);
            start.requestFocus();
        });
        row.addView(done);
        gameModeCard.addView(row, new LinearLayout.LayoutParams(-1, -2));
        return gameModeCard;
    }

    private View buildUpdateCard() {
        updateCard = new LinearLayout(this);
        updateCard.setOrientation(LinearLayout.VERTICAL);
        updateCard.setPadding(dp(16), dp(12), dp(16), dp(12));
        updateCard.setBackground(Ui.rect(this, Ui.alpha(Ui.PANEL, 0xe0), Ui.CYAN, 2, 16));
        updateTitle = Ui.text(this, "", 15, Ui.CYAN, true);
        updateCard.addView(updateTitle);
        updateHow = Ui.text(this, "", 12.5f, Ui.TEXT, false);
        updateHow.setPadding(0, dp(6), 0, dp(8));
        updateCard.addView(updateHow);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        TextView later = Ui.button(this, Tr.t("Later"), false);
        later.setOnClickListener(v -> { updateCard.setVisibility(View.GONE); start.requestFocus(); });
        row.addView(later);
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(10), 1));
        TextView notes = Ui.button(this, Tr.t("What's new"), false);
        notes.setOnClickListener(v -> Updates.open(this, updateUrl));
        row.addView(notes);
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(10), 1));
        updateNow = Ui.button(this, Tr.t("Update now"), true);
        updateNow.setOnClickListener(v -> updateNow());
        row.addView(updateNow);
        updateCard.addView(row, new LinearLayout.LayoutParams(-1, -2));
        updateCard.setVisibility(View.GONE);
        return updateCard;
    }

    // The card: a newer release (Update now), or, right after an update, SET UP again for its new runtime.
    private void showUpdate() {
        if (updateCard == null || downloading) return;
        if (update != null) {
            updateTitle.setText("⬆  " + Tr.f("Update available: %s", update.tag));
            updateHow.setText(Tr.f("Update now downloads the new app (%s MB) and opens the installer. Your game "
                    + "files and saves stay; afterwards tap SET UP again (a few minutes).", update.apkSize / 1000000));
            updateNow.setText(Tr.t("Update now"));
            updateCard.setVisibility(View.VISIBLE);
        } else if (Setup.onDevice(this) && Setup.builtByOtherVersion(this)) {
            updateTitle.setText("✓  " + Tr.f("Updated to %s", Report.appVersion(this)));
            updateHow.setText(Tr.t("Tap SET UP again so the game gets the new version's fixes. It picks up where "
                    + "the last one ended: only what changed is rebuilt."));
            updateNow.setText(Tr.t("SET UP again"));
            updateCard.setVisibility(View.VISIBLE);
        } else {
            updateCard.setVisibility(View.GONE);
        }
    }

    private void updateNow() {
        if (update == null) {  // SET UP again after an update
            startActivity(new Intent(this, SetupActivity.class));
            return;
        }
        if (downloading) return;
        if (!getPackageManager().canRequestPackageInstalls()) {  // Android asks once: may this app install apps?
            Toast.makeText(this, Tr.t("Allow installing apps from Bayonetta 3 Recomp, then come back and tap "
                    + "Update now again."), Toast.LENGTH_LONG).show();
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception ignored) {
            }
            return;
        }
        downloading = true;
        updateNow.setEnabled(false);
        updateNow.setAlpha(0.5f);
        Updates.download(this, update, (done, total) -> updateHow.setText(
                Tr.f("Downloading the new app: %1$d / %2$d MB", done / 1000000, total / 1000000)), (apk, error) -> {
            downloading = false;
            updateNow.setEnabled(true);
            updateNow.setAlpha(1f);
            if (apk == null) {
                updateHow.setText(Tr.f("The download stopped: %s. Tap Update now to try again.", error));
                return;
            }
            updateHow.setText(Tr.t("Downloaded. Confirm the install; afterwards open the app and tap SET UP again."));
            Updates.install(this, apk);
        });
    }

    private LinearLayout.LayoutParams gameModeParams() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(12);
        return lp;
    }

    private void showNote() {
        String mode = gameMode();
        note.setText(Tr.t("Many phones (vivo, Xiaomi, OPPO, Honor, Samsung and others) slow down apps they do not know "
                + "as games. Add Bayonetta 3 Recomp in your phone's Game Center, Game Booster, Game Space or game mode "
                + "before you play, or it cannot run at full speed.")
                + (mode != null ? "\n" + Tr.f("Game mode now: %s", Tr.t(mode)) : ""));
        gameModeCard.setVisibility(getSharedPreferences("launcher", MODE_PRIVATE).getBoolean("game_mode_done", false)
                ? View.GONE : View.VISIBLE);
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
            Toast.makeText(this, Tr.f("Could not write config.txt: %s", e.getMessage()), Toast.LENGTH_LONG).show();
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

    // Settings > Game folder: Internal storage or an SD card (Storage.java). A folder that already holds the game is
    // just used; the game moves there when it is here (copied, then removed here); with nothing set up yet, SET UP
    // puts it there.
    private void pickFolder() {
        if (Setup.current() != null && Setup.current().running) {
            Toast.makeText(this, Tr.t("The setup is running: change the game folder after it."), Toast.LENGTH_LONG).show();
            return;
        }
        final List<Storage.Volume> vols = Storage.volumes(this);
        if (vols.size() < 2) {
            Toast.makeText(this, Tr.t("No SD card found: insert one (exFAT) and come back."), Toast.LENGTH_LONG).show();
            return;
        }
        Storage.Volume cur = Storage.volumeOf(this, data.root);
        String[] names = new String[vols.size()];
        for (int i = 0; i < vols.size(); i++) {
            Storage.Volume v = vols.get(i);
            names[i] = v.name + (cur != null && v.dir.equals(cur.dir) ? "  " + Tr.t("(now)") : "") + " · "
                    + Tr.f("%s free", DataCheck.size(v.free()))
                    + (DataCheck.hasData(v.gameFolder()) ? " · " + Tr.t("holds the game") : "");
        }
        new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle(Tr.t("Game folder"))
                .setItems(names, (d, w) -> folderChosen(vols.get(w), cur))
                .setNegativeButton(Tr.t("Cancel"), null)
                .show();
    }

    private void folderChosen(Storage.Volume to, Storage.Volume cur) {
        if (cur != null && to.dir.equals(cur.dir)) return;
        File dest = to.gameFolder();
        boolean ownFolder = data.root.equals(DataCheck.ownRoot(this));
        if (DataCheck.hasData(dest)) {
            new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                    .setTitle(Tr.f("Use the game on %s?", to.name))
                    .setMessage(Tr.f("%s/%s already holds the game data: the game runs from there, with the saves and "
                            + "settings in it. %s stays as it is.", to.name, DataCheck.TITLE, data.rootLabel()))
                    .setPositiveButton(Tr.t("Use it"), (d, w) -> switchFolder(to))
                    .setNegativeButton(Tr.t("Cancel"), null)
                    .show();
        } else if (data.folderExists && DataCheck.hasData(data.root) && !ownFolder) {
            final long total = Storage.size(data.root), free = to.free();
            String size = DataCheck.size(total);
            AlertDialog.Builder b = new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                    .setTitle(Tr.f("Move the game to %s?", to.name));
            if (free < total + (256L << 20)) {
                b.setMessage(Tr.f("The game folder is %s and %s has %s free. Free some space there first.", size,
                        to.name, DataCheck.size(free))).setPositiveButton(Tr.t("Close"), null).show();
                return;
            }
            b.setMessage(Tr.f("This copies the game folder (%s: the game data, your saves, settings and shader cache) "
                    + "to %s/%s, then removes it from %s. About %d minutes on a fast card; if it stops half way, the "
                    + "game stays where it is. Keep the phone charging.", size, to.name, DataCheck.TITLE,
                    data.rootLabel(), Math.max(1, total / (40L << 20) / 60)))
                    .setPositiveButton(Tr.t("Move"), (d, w) -> moveFolder(to))
                    .setNegativeButton(Tr.t("Cancel"), null)
                    .show();
        } else {
            switchFolder(to);
            Toast.makeText(this, needsSetup() ? Tr.f("SET UP puts the game on %s.", to.name)
                    : Tr.f("The game folder is now on %s.", to.name), Toast.LENGTH_LONG).show();
        }
    }

    private void switchFolder(Storage.Volume to) {
        try {
            Storage.choose(this, to);
        } catch (IOException e) {
            Toast.makeText(this, Tr.f("Could not change the game folder: %s", e.getMessage()), Toast.LENGTH_LONG).show();
        }
        refresh();
    }

    private void moveFolder(Storage.Volume to) {
        final Storage.Move move = new Storage.Move(data.root, to.gameFolder());
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(12), dp(24), 0);
        final TextView what = Ui.text(this, Tr.t("Starting"), 13, Ui.MUTED, false);
        box.addView(what);
        final android.widget.ProgressBar bar = new android.widget.ProgressBar(this, null,
                android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        box.addView(bar, new LinearLayout.LayoutParams(-1, dp(24)));
        final AlertDialog dialog = new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setTitle(Tr.f("Moving the game to %s", to.name))
                .setView(box)
                .setCancelable(false)
                .setNegativeButton(Tr.t("Stop"), (d, w) -> move.cancelled = true)
                .show();
        new Thread(() -> {
            String error = null;
            try {
                move.run(this, to, (done, total, file) -> ui.post(() -> {
                    bar.setProgress(total > 0 ? (int) (1000L * done / total) : 0);
                    what.setText(Tr.f("%s of %s", DataCheck.size(done), DataCheck.size(total)) + " · " + file);
                }));
            } catch (IOException e) {
                error = e.getMessage();
            }
            final String err = error;
            ui.post(() -> {
                dialog.dismiss();
                refresh();
                Toast.makeText(this, err == null ? Tr.f("The game is now on %s.", to.name)
                        : Tr.f("Not moved: %s. The game is still in %s.", err, data.rootLabel()),
                        Toast.LENGTH_LONG).show();
            });
        }, "b3-move").start();
    }

    private boolean needsSetup() {
        return Setup.onDevice(this) && (!Setup.gameLibrary(this).isFile() || !data.ready()
                || (Setup.current() != null && Setup.current().running));
    }

    private void startGame() {
        if (cfg == null) return;
        // The same check as at launch, with the free space when this starts a setup.
        DeviceCheck.Result r = DeviceCheck.run(this, gpu, needsSetup());
        if (!r.ok() && !tried(r)) {
            specDialog(r, this::startGame);
            return;
        }
        if (needsSetup()) {
            startActivity(new Intent(this, SetupActivity.class));
            return;
        }
        if (!data.ready()) {
            Toast.makeText(this, Tr.t("The game data is incomplete: the list on the left says what is missing."),
                    Toast.LENGTH_LONG).show();
            return;
        }
        if (panelLayer.getVisibility() == View.VISIBLE) saveAdvanced();
        if (!cfg.values.containsKey("NX_PROFILE")) cfg.setProfile(cfg.profile());
        if (!save()) return;
        startActivity(new Intent(this, GameActivity.class));
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
        return Report.text(this, "launcher", gpu, data, cfg, String.valueOf(padValue.getText()));
    }

    private void shareReport() { makeReport(false); }

    // Report a bug: Report.java (the zip to Downloads and the filled-in issue page, else the share sheet).
    private void reportIssue() { makeReport(true); }

    private void makeReport(boolean issue) { Report.make(this, report(), data != null ? data.root : null, issue, gpu); }
}
