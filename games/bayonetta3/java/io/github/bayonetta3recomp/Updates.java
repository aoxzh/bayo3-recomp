package io.github.bayonetta3recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Updates from this project's GitHub releases. When the launcher opens it asks the public releases API whether a
 *  newer release is out (Settings > Check for updates, on by default; only the request goes out, with a generic
 *  User-Agent and no phone details). Update now downloads that release's APK into the app's cache and hands it to
 *  Android's installer, which shows its own confirmation and installs it only when it is signed with the same key as
 *  this app. Nothing is downloaded or installed without a tap. Also the author's X link. */
final class Updates {
    static final String API = "https://api.github.com/repos/aoxzh/bayo3-recomp/releases/latest";
    static final String RELEASES = "https://github.com/aoxzh/bayo3-recomp/releases/latest";
    static final String AUTHOR = "https://x.com/aoxzh";
    static final String PREF = "update_check";
    static final String USER_AGENT = "bayo3-recomp-launcher";

    static final class Release {
        final String tag, page, apkUrl;
        final long apkSize;
        Release(String tag, String page, String apkUrl, long apkSize) {
            this.tag = tag; this.page = page; this.apkUrl = apkUrl; this.apkSize = apkSize;
        }
    }

    interface Found { void found(Release r); }
    interface Progress { void at(long done, long total); }
    interface Done { void done(File apk, String error); }

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

    private static HttpURLConnection open(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(10000);
        c.setReadTimeout(20000);
        c.setRequestProperty("User-Agent", USER_AGENT);
        c.setInstanceFollowRedirects(true);  // release assets redirect to GitHub's file host (https to https)
        return c;
    }

    /** On its own thread; `cb` runs on the UI thread, only when a newer release with an APK is out. */
    static void check(final Activity a, final Found cb) {
        if (!enabled(a)) return;
        final String current = Report.appVersion(a);
        if (parse(current) == null) return;  // a development build: nothing to compare
        final Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            HttpURLConnection c = null;
            try {
                c = open(API);
                c.setRequestProperty("Accept", "application/vnd.github+json");
                if (c.getResponseCode() != 200) return;
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                try (InputStream in = c.getInputStream()) {
                    byte[] buf = new byte[8192];
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                JSONObject j = new JSONObject(out.toString("UTF-8"));
                String tag = j.optString("tag_name", "");
                if (!newer(tag, current)) return;
                String apk = null;
                long size = 0;
                JSONArray assets = j.optJSONArray("assets");
                for (int i = 0; assets != null && i < assets.length(); i++) {
                    JSONObject x = assets.getJSONObject(i);
                    if (x.optString("name", "").endsWith("-android-arm64.apk")) {
                        apk = x.optString("browser_download_url", null);
                        size = x.optLong("size", 0);
                    }
                }
                if (apk == null || !apk.startsWith("https://github.com/aoxzh/bayo3-recomp/releases/download/")) return;
                final Release r = new Release(tag, j.optString("html_url", RELEASES), apk, size);
                ui.post(() -> { if (!a.isFinishing()) cb.found(r); });
            } catch (Exception ignored) {
                // offline or GitHub unreachable: say nothing
            } finally {
                if (c != null) c.disconnect();
            }
        }, "update-check").start();
    }

    /** The release's APK into the app's cache (SupportProvider.dir), on its own thread; progress and done run on the
     *  UI thread. A download that ends short of the release's size is an error. */
    static void download(final Activity a, final Release r, final Progress progress, final Done done) {
        final Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            File dir = SupportProvider.dir(a);
            dir.mkdirs();
            File apk = new File(dir, "update-" + r.tag.replaceAll("[^A-Za-z0-9.]", "") + ".apk");
            File part = new File(dir, apk.getName() + ".part");
            HttpURLConnection c = null;
            try {
                if (apk.isFile() && apk.length() == r.apkSize) {  // downloaded before: the installer was closed
                    ui.post(() -> done.done(apk, null));
                    return;
                }
                c = open(r.apkUrl);
                if (c.getResponseCode() != 200) throw new Exception("HTTP " + c.getResponseCode());
                long total = r.apkSize > 0 ? r.apkSize : c.getContentLengthLong();
                long got = 0, shown = 0;
                try (InputStream in = c.getInputStream(); OutputStream o = new FileOutputStream(part)) {
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; ) {
                        o.write(buf, 0, n);
                        got += n;
                        if (got - shown > (1 << 20)) {
                            shown = got;
                            final long g = got;
                            ui.post(() -> progress.at(g, total));
                        }
                    }
                }
                if (r.apkSize > 0 && got != r.apkSize) throw new Exception("incomplete (" + got + " of " + r.apkSize + " bytes)");
                if (!part.renameTo(apk)) throw new Exception("cannot save " + apk.getName());
                ui.post(() -> done.done(apk, null));
            } catch (Exception e) {
                part.delete();
                final String why = e.getMessage() != null ? e.getMessage() : e.toString();
                ui.post(() -> done.done(null, why));
            } finally {
                if (c != null) c.disconnect();
            }
        }, "update-download").start();
    }

    /** Android's installer for the downloaded APK (it confirms with the player and checks the signature). */
    static void install(Activity a, File apk) {
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(SupportProvider.uri(a, apk), "application/vnd.android.package-archive");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            a.startActivity(i);
        } catch (Exception e) {
            Toast.makeText(a, e.toString(), Toast.LENGTH_LONG).show();
        }
    }

    static void open(Activity a, String url) {
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (Exception e) {
            Toast.makeText(a, url, Toast.LENGTH_LONG).show();
        }
    }
}
