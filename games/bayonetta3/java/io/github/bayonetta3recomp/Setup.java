package io.github.bayonetta3recomp;

import android.content.Context;
import android.content.pm.PackageInfo;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// The on-device setup (an APK made by android/build_ondevice_apk.py): the player's own dump in Internal storage/
// Bayonetta 3/dump becomes the game on the phone, no PC. The APK carries tools only, never game code:
//   lib/arm64-v8a/        clang, lld, python, the shader translator, spirv-val, the game loader (libb3loader.so)
//   assets/ondevice.zip   the sysroot, clang's headers, the runtime archive, Python's library and this project's
//                         tools, unpacked once per APK version into <files>/b3
// tools/b3_win.py setup runs here as on a PC (its Android branch): the data moved into the data root, the recompiler,
// the shaders, and the compile and link of <files>/game/libbayonetta3.so, which libb3loader.so loads at Start.
// One run at a time, owned by this class, so the screen can go away and come back to it.
final class Setup {
    static final String ASSET = "ondevice.zip";

    interface Listener { void changed(); }

    private static Setup current;

    final List<String> lines = new ArrayList<>();
    String phase = "Starting";
    int done = -1, total = -1;
    boolean running = true, ok;
    String error;
    private Process process;
    private Listener listener;

    private static Boolean onDevice;

    static synchronized boolean onDevice(Context c) {
        if (onDevice == null) {
            try (InputStream in = c.getAssets().open(ASSET)) {
                onDevice = true;
            } catch (IOException e) {
                onDevice = false;
            }
        }
        return onDevice;
    }

    static File gameLibrary(Context c) { return new File(c.getFilesDir(), "game/libbayonetta3.so"); }

    static File dumpFolder() {
        File found = findDump();
        return found != null ? found : new File(DataCheck.sharedRoot(), "dump");
    }

    // Where the player's extraction is: Bayonetta 3/dump first, then Download, Documents and the top of Internal
    // storage, a few folder levels down; any folder holding an ExeFS (main) and a RomFS (.pkz files), in any of the
    // layouts tools/b3_dump.py resolves (ExeFS/RomFS in any case, a folder level more, romfs/romfs). The setup then
    // moves the files from there into the game's folder (the same storage: no copy).
    static File findDump() {
        File sd = android.os.Environment.getExternalStorageDirectory();
        File[] roots = {new File(DataCheck.sharedRoot(), "dump"), new File(sd, "Download"), new File(sd, "Documents"),
                        sd};
        for (File r : roots) {
            File hit = search(r, 0);
            if (hit != null) return hit;
        }
        return null;
    }

    private static File search(File dir, int depth) {
        if (dir == null || !dir.isDirectory()) return null;
        if (has(dir, "exefs", true) && has(dir, "romfs", false)) return dir;
        if (depth >= 3) return null;
        File[] kids = dir.listFiles();
        if (kids == null) return null;
        for (File k : kids) {
            String n = k.getName();
            if (!k.isDirectory() || n.startsWith(".") || n.equalsIgnoreCase("Android")
                    || n.equals(DataCheck.TITLE) && depth == 0 && dir.equals(android.os.Environment.getExternalStorageDirectory()))
                continue;
            File hit = search(k, depth + 1);
            if (hit != null) return hit;
        }
        return null;
    }

    // An exefs/romfs child (any case) holding the ExeFS main or .pkz files, also one level further (romfs/romfs).
    private static boolean has(File dir, String name, boolean exefs) {
        File[] kids = dir.listFiles();
        if (kids == null) return false;
        for (File k : kids) {
            if (!k.isDirectory() || !k.getName().equalsIgnoreCase(name)) continue;
            if (holds(k, exefs)) return true;
            File[] inner = k.listFiles();
            if (inner != null)
                for (File i : inner) if (i.isDirectory() && i.getName().equalsIgnoreCase(name) && holds(i, exefs)) return true;
        }
        return false;
    }

    private static boolean holds(File dir, boolean exefs) {
        String[] names = dir.list();
        if (names == null) return false;
        for (String n : names) {
            if (exefs && n.equalsIgnoreCase("main")) return true;
            if (!exefs && n.toLowerCase(java.util.Locale.ROOT).endsWith(".pkz")) return true;
        }
        return false;
    }

    // What is missing, in plain words, or null when an extraction was found (b3_win.py checks the files themselves:
    // version 1.2.0, a complete RomFS).
    static String dumpProblem() {
        if (findDump() != null) return null;
        return "no extracted game found. Extract the ExeFS and the RomFS of your Bayonetta 3 (update 1.2.0 "
                + "installed) with your emulator (Ryujinx: right-click > Extract Data > ExeFS, then RomFS; Eden / "
                + "yuzu: Dump RomFS > Full, and Dump ExeFS) and copy the folder with exefs and romfs to the phone, "
                + "for example into Internal storage/" + DataCheck.TITLE + "/dump or Download.";
    }

    static synchronized Setup current() { return current; }

    static synchronized Setup start(Context app) {
        if (current != null && current.running) return current;
        current = new Setup();
        Setup s = current;
        new Thread(() -> s.run(app.getApplicationContext()), "b3-setup").start();
        SetupService.start(app.getApplicationContext());  // a notification, and it goes on with the screen off
        return s;
    }

    synchronized void listen(Listener l) { listener = l; }

    void cancel() {
        Process p;
        synchronized (this) { p = process; }
        if (p != null) p.destroy();
    }

    private void changed() {
        Listener l;
        synchronized (this) { l = listener; }
        if (l != null) l.changed();
    }

    private void line(String s) {
        synchronized (this) {
            lines.add(s);
            if (lines.size() > 400) lines.subList(0, lines.size() - 400).clear();
            if (s.startsWith("b3: ")) {
                String m = s.substring(4);
                int colon = m.indexOf(':');
                phase = colon > 0 ? m.substring(0, colon) : m;
                // "build: 120/1200 files" and the shader tool's "translated batch 3/58"
                java.util.regex.Matcher x = java.util.regex.Pattern.compile("(\\d+)/(\\d+)").matcher(m);
                if (x.find()) { done = Integer.parseInt(x.group(1)); total = Integer.parseInt(x.group(2)); }
                else { done = -1; total = -1; }
            } else if (s.startsWith("translated batch ")) {
                java.util.regex.Matcher x = java.util.regex.Pattern.compile("(\\d+)/(\\d+)").matcher(s);
                if (x.find()) { done = Integer.parseInt(x.group(1)); total = Integer.parseInt(x.group(2)); }
            }
        }
        changed();
    }

    private void run(Context c) {
        File log = new File(DataCheck.sharedRoot(), "setup.log");
        try (Writer w = new FileWriter(log, true)) {
            File b3 = new File(c.getFilesDir(), "b3");
            unpack(c, b3);
            String lib = c.getApplicationInfo().nativeLibraryDir;
            List<String> cmd = new ArrayList<>();
            cmd.add(lib + "/libb3python.so");
            cmd.add("-u");
            cmd.add(new File(b3, "repo/tools/b3_win.py").getPath());
            cmd.add("setup");
            cmd.add("--dump");
            cmd.add(dumpFolder().getPath());
            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.directory(new File(b3, "repo"));
            pb.redirectErrorStream(true);
            Map<String, String> env = pb.environment();
            env.put("PYTHONHOME", new File(b3, "python").getPath());
            env.put("PYTHONDONTWRITEBYTECODE", "1");
            env.put("LD_LIBRARY_PATH", lib);
            env.put("HOME", b3.getPath());
            env.put("TMPDIR", c.getCacheDir().getPath());
            env.put("B3_ANDROID_LIB", lib);
            env.put("B3_ANDROID_DATA", DataCheck.sharedRoot().getPath());
            env.put("B3_ANDROID_GAME", gameLibrary(c).getParent());
            env.put("B3_WIN_ROOT", new File(b3, "win").getPath());
            env.put("B3_TOOLSET", new File(b3, "toolset").getPath());
            env.put("B3_DLMAIN", lib + "/libb3dlmain.so");
            line("b3: setup: " + String.join(" ", cmd));
            Process p = pb.start();
            synchronized (this) { process = p; }
            ArrayDeque<String> tail = new ArrayDeque<>();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                String s;
                while ((s = r.readLine()) != null) {
                    w.write(s + "\n");
                    w.flush();
                    line(s);
                    tail.add(s);
                    if (tail.size() > 3) tail.removeFirst();
                }
            }
            int code = p.waitFor();
            synchronized (this) {
                ok = code == 0 && gameLibrary(c).isFile();
                if (!ok) error = "setup stopped (exit " + code + "): " + String.join(" / ", tail);
            }
        } catch (Exception e) {
            synchronized (this) { error = e.toString(); }
            line("error: " + e);
        } finally {
            synchronized (this) { running = false; process = null; }
            changed();
        }
    }

    // assets/ondevice.zip into <files>/b3, once per installed APK (its update time names the unpacked copy). The
    // tools' work (<files>/b3/win: the generated code, the objects) is kept across updates.
    private void unpack(Context c, File b3) throws Exception {
        PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
        String id = Long.toString(pi.lastUpdateTime);
        File stamp = new File(b3, "unpacked.id");
        if (stamp.isFile() && id.equals(read(stamp))) return;
        synchronized (this) { phase = "Unpacking the tools"; }
        line("b3: unpack: the setup tools (about a minute)");
        for (String part : new String[] {"repo", "toolset", "python"}) deleteTree(new File(b3, part));
        byte[] buf = new byte[1 << 16];
        int n = 0;
        try (ZipInputStream z = new ZipInputStream(c.getAssets().open(ASSET))) {
            ZipEntry e;
            String canon = b3.getCanonicalPath() + File.separator;
            while ((e = z.getNextEntry()) != null) {
                File f = new File(b3, e.getName());
                if (!f.getCanonicalPath().startsWith(canon)) throw new IOException("bad entry " + e.getName());
                if (e.isDirectory()) { f.mkdirs(); continue; }
                f.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(f)) {
                    int k;
                    while ((k = z.read(buf)) > 0) o.write(buf, 0, k);
                }
                if (++n % 2000 == 0) line("b3: unpack: " + n + " files");
            }
        }
        try (Writer w = new FileWriter(stamp)) { w.write(id); }
    }

    private static String read(File f) {
        try (BufferedReader r = new BufferedReader(new java.io.FileReader(f))) {
            String s = r.readLine();
            return s == null ? "" : s.trim();
        } catch (IOException e) {
            return "";
        }
    }

    private static void deleteTree(File f) {
        File[] in = f.listFiles();
        if (in != null && !isLink(f)) for (File x : in) deleteTree(x);
        f.delete();
    }

    private static boolean isLink(File f) { return java.nio.file.Files.isSymbolicLink(f.toPath()); }
}
