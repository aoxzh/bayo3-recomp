package io.github.bayonetta3recomp;

import android.app.Activity;
import android.app.GameManager;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.view.InputDevice;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.BufferedReader;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

// The bug report, from the launcher's Report a bug / Share a bug report and from the in-game menu (GameActivity): a zip
// of chip-and-app.txt, the end of nx.log and config.txt. With `issue`, the zip goes to Downloads and our GitHub issue
// page opens with its form filled in (what to write, the app, Android, chip and GPU: no paths); the player signs in,
// writes what happened, attaches the zip and submits. Without Downloads, or for a share, it goes to the share sheet.
final class Report {
    private Report() {}

    // The GPU name: ChipInfo.gpu() makes an EGL context, which the launcher does once and keeps here, so the game's
    // process (GameActivity, Vulkan in use) never has to.
    private static File gpuFile(Context c) { return new File(c.getFilesDir(), "gpu.txt"); }

    static void keepGpu(Context c, String gpu) {
        if (gpu == null) return;
        try (Writer w = new OutputStreamWriter(new FileOutputStream(gpuFile(c)), StandardCharsets.UTF_8)) {
            w.write(gpu + "\n");
        } catch (IOException ignored) {
        }
    }

    static String keptGpu(Context c) {
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(gpuFile(c)),
                StandardCharsets.UTF_8))) {
            String s = r.readLine();
            return s == null || s.trim().isEmpty() ? null : s.trim();
        } catch (IOException e) {
            return null;
        }
    }

    static String appVersion(Context c) {
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return pi.versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    static String gameMode(Context c) {
        if (Build.VERSION.SDK_INT < 31) return null;
        GameManager gm = c.getSystemService(GameManager.class);
        if (gm == null) return null;
        switch (gm.getGameMode()) {
            case GameManager.GAME_MODE_STANDARD: return "Standard";
            case GameManager.GAME_MODE_PERFORMANCE: return "Performance";
            case GameManager.GAME_MODE_BATTERY: return "Battery";
            case 4: return "Custom";
            default: return null;
        }
    }

    // The connected controllers by name. A controller number is what the system gives real pads (built-in ones too);
    // vendor virtual devices that report joystick classes (force-feedback nodes and the like) have none.
    static List<String> pads() {
        List<String> names = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual() || d.getControllerNumber() <= 0) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                if (!names.contains(d.getName())) names.add(d.getName());
        }
        return names;
    }

    // chip-and-app.txt. `from`: "launcher" or "in-game"; `controllers` as the launcher shows them.
    static String text(Context c, String from, String gpu, DataCheck data, Config cfg, String controllers) {
        StringBuilder b = new StringBuilder();
        String version = "?";
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            version = pi.versionName + " (" + pi.getLongVersionCode() + ")";
        } catch (Exception ignored) {
        }
        b.append("Bayonetta 3 Recomp: ").append(from).append(" report, ")
                .append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date())).append('\n');
        b.append("app: ").append(version).append('\n');
        b.append("android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        b.append("soc: ").append(ChipInfo.soc()).append('\n');
        b.append("gpu: ").append(gpu == null ? "?" : gpu).append('\n');
        b.append("abi: ").append(Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?").append('\n');
        b.append("cpus: ").append(Runtime.getRuntime().availableProcessors()).append('\n');
        PowerManager pm = c.getSystemService(PowerManager.class);
        b.append("thermal status: ").append(pm == null ? -1 : pm.getCurrentThermalStatus()).append('\n');
        String mode = gameMode(c);
        b.append("game mode: ").append(mode == null ? "unsupported" : mode).append('\n');
        b.append("all-files access: ").append(Environment.isExternalStorageManager()).append('\n');
        if (data != null) {
            b.append("data folder: ").append(data.root).append(data.ready() ? " (ready)" : " (incomplete)").append('\n');
            for (DataCheck.Item i : data.missing) b.append("  missing: ").append(i.path).append('\n');
            b.append("pipeline cache: ").append(data.cacheFiles).append(" file(s), ")
                    .append(DataCheck.size(data.cacheBytes)).append('\n');
        }
        if (cfg != null) b.append("profile: ").append(cfg.profile()).append('\n');
        b.append("controllers: ").append(controllers).append('\n');
        return b.toString();
    }

    private static void openIssue(Activity a, String zipName, String gpu) {
        // The issue form (.github/ISSUE_TEMPLATE/bug.yml): What happened and Where are required there, so an
        // issue always says something; the zip's name and the phone's details are filled in.
        String device = "app: " + appVersion(a) + "\n"
                + "android: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + "chip: " + ChipInfo.soc() + "\n"
                + "GPU: " + (gpu == null ? "?" : gpu);
        Uri uri = Uri.parse("https://github.com/aoxzh/bayo3-recomp/issues/new").buildUpon()
                .appendQueryParameter("template", "bug.yml")
                .appendQueryParameter("title", "[Bug][Android] ")
                .appendQueryParameter("report", "Attach " + zipName + " from your phone's Downloads folder "
                        + "(logs and settings, no game files).")
                .appendQueryParameter("device", device).build();
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, uri));
        } catch (Exception e) {
            Toast.makeText(a, Tr.t("No browser found: open github.com/aoxzh/bayo3-recomp/issues"), Toast.LENGTH_LONG).show();
        }
    }

    // Makes the zip on its own thread, then opens the issue page (issue) or the share sheet.
    static void make(final Activity a, final String text, final File root, final boolean issue, final String gpu) {
        final Handler ui = new Handler(Looper.getMainLooper());
        Toast.makeText(a, Tr.t("Preparing the report…"), Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            File dir = SupportProvider.dir(a);
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
            File saved = null;
            if (issue && error == null) {
                File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                File copy = new File(downloads, zip.getName());
                try (FileInputStream in = new FileInputStream(zip); FileOutputStream out = new FileOutputStream(copy)) {
                    byte[] buf = new byte[1 << 16];
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                    saved = copy;
                } catch (IOException ignored) {  // no access to Downloads: the share sheet below
                }
            }
            final String err = error;
            final File inDownloads = saved;
            ui.post(() -> {
                if (err != null) {
                    Toast.makeText(a, Tr.f("Could not make the report: %s", err), Toast.LENGTH_LONG).show();
                    return;
                }
                if (inDownloads != null) {
                    Toast.makeText(a, Tr.f("Saved %s in Downloads: attach it on the page that opens",
                            inDownloads.getName()), Toast.LENGTH_LONG).show();
                    openIssue(a, inDownloads.getName(), gpu);
                    return;
                }
                Uri uri = SupportProvider.uri(a, zip);
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType("application/zip");
                send.putExtra(Intent.EXTRA_STREAM, uri);
                send.putExtra(Intent.EXTRA_SUBJECT, "Bayonetta 3 Recomp bug report");
                send.putExtra(Intent.EXTRA_TEXT, "What happened, and what you were doing:\n\n");
                send.setClipData(ClipData.newRawUri(zip.getName(), uri));
                send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                a.startActivity(Intent.createChooser(send, Tr.t("Send the bug report")));
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
