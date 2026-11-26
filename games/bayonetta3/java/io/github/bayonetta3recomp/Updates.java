package io.github.bayonetta3recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Is a newer release out? The launcher asks GitHub's public releases API once when it opens (Settings > Check for
 *  updates, on by default). Nothing but the request goes out (a generic User-Agent, no phone details), and nothing is
 *  downloaded or installed by itself: the card only opens the release page. Also the author's X link. */
final class Updates {
    static final String API = "https://api.github.com/repos/aoxzh/bayo3-recomp/releases/latest";
    static final String RELEASES = "https://github.com/aoxzh/bayo3-recomp/releases/latest";
    static final String AUTHOR = "https://x.com/aoxzh";
    static final String PREF = "update_check";

    interface Found { void found(String tag, String url); }

    private Updates() {}

    static boolean enabled(Activity a) {
        return a.getSharedPreferences("launcher", Activity.MODE_PRIVATE).getBoolean(PREF, true);
    }

    static void setEnabled(Activity a, boolean on) {
        a.getSharedPreferences("launcher", Activity.MODE_PRIVATE).edit().putBoolean(PREF, on).apply();
    }

    static int[] parse(String v) {
        Matcher m = Pattern.compile("v?(\\d+)\\.(\\d+)\\.(\\d+)").matcher(v == null ? "" : v.trim());
        if (!m.lookingAt()) return null;
        return new int[] {Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2)), Integer.parseInt(m.group(3))};
    }

    static boolean newer(String latest, String current) {
        int[] a = parse(latest), b = parse(current);
        if (a == null || b == null) return false;
        for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
        return false;
    }

    /** On its own thread; `cb` runs on the UI thread, only when a newer release than this app is out. */
    static void check(final Activity a, final Found cb) {
        if (!enabled(a)) return;
        final String current = Report.appVersion(a);
        if (parse(current) == null) return;  // a development build: nothing to compare
        final Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection) new URL(API).openConnection();
                c.setConnectTimeout(6000);
                c.setReadTimeout(6000);
                c.setRequestProperty("Accept", "application/vnd.github+json");
                c.setRequestProperty("User-Agent", "bayo3-recomp-launcher");
                if (c.getResponseCode() != 200) return;
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                try (InputStream in = c.getInputStream()) {
                    byte[] buf = new byte[8192];
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                JSONObject j = new JSONObject(out.toString("UTF-8"));
                final String tag = j.optString("tag_name", "");
                final String url = j.optString("html_url", RELEASES);
                if (newer(tag, current)) ui.post(() -> { if (!a.isFinishing()) cb.found(tag, url); });
            } catch (Exception ignored) {
                // offline or GitHub unreachable: say nothing
            } finally {
                if (c != null) c.disconnect();
            }
        }, "update-check").start();
    }

    static void open(Activity a, String url) {
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            Toast.makeText(a, url, Toast.LENGTH_LONG).show();
        }
    }
}
