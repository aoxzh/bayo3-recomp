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

// The on-device setup (an APK made by android/build_ondevice_apk.py): the player's own dump (Bayonetta 3/dump, Download,
// ... on Internal storage or the SD card) becomes the game on the phone, no PC. The APK carries tools only, never game code:
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

    // The app version that built (or received) the game. An update of the app brings a new runtime, which reaches
    // the game only through SET UP again (it resumes: only what changed is rebuilt).
    static File builtStamp(Context c) { return new File(c.getFilesDir(), "game/built_with.txt"); }

    static void markBuilt(Context c) { markBuilt(c, ""); }

    /** how: "" built here, " pc" received from a computer (tools/b3_win.py phone). */
    static void markBuilt(Context c, String how) {
        try (Writer w = new FileWriter(builtStamp(c))) { w.write(Report.appVersion(c) + how); } catch (IOException ignored) {}
    }

    /** The game was set up on this phone (the tools' state is there): a new version needs only
     *  `b3_win.py update`, as the desktop's Rebuild: no dump (the first setup moved it), only what changed. */
    static boolean refreshOnly(Context c) {
        return gameLibrary(c).isFile() && new File(c.getFilesDir(), "b3/win/state.json").isFile();
    }

    /** The game came from a computer (Send to my phone): a new version is sent the same way. A game with no setup
     *  state here came from one too, also from a version before the stamp. */
    static boolean fromComputer(Context c) {
        return gameLibrary(c).isFile() && !refreshOnly(c);
    }

    /** The game is there but was built by another version of this app (a release, not a development build). */
    static boolean builtByOtherVersion(Context c) {
        String v = Report.appVersion(c);
        if (!gameLibrary(c).isFile() || Updates.parse(v) == null) return false;
        String stamp = builtStamp(c).isFile() ? read(builtStamp(c)) : null;
        if (stamp != null && stamp.endsWith(" pc")) stamp = stamp.substring(0, stamp.length() - 3);
        return !v.equals(stamp);
    }

    static File dumpFolder(Context c) {
        File found = findDump(c);
        return found != null ? found : new File(DataCheck.target(c), "dump");
    }

    // Where the player's extraction is: on the game folder's own volume first (Internal storage or the SD card picked
    // in Settings), then on the others; on each, Bayonetta 3/dump, then Download, Documents and the top, a few folder
    // levels down; any folder holding an ExeFS (main) and a RomFS (.pkz files), in any of the layouts tools/b3_dump.py
    // resolves (ExeFS/RomFS in any case, a folder level more, romfs/romfs). The setup then moves the files from there
    // into the game's folder: a rename on the same volume, a copy (and as much free space) from another one.
    static File findDump(Context c) {
        File target = DataCheck.target(c);
        List<Storage.Volume> order = new ArrayList<>();
        Storage.Volume home = Storage.volumeOf(c, target);
        if (home != null) order.add(home);
        for (Storage.Volume v : Storage.volumes(c)) if (home == null || !v.dir.equals(home.dir)) order.add(v);
        List<File> roots = new ArrayList<>();
        roots.add(new File(target, "dump"));
        for (Storage.Volume v : order) {
            roots.add(new File(v.gameFolder(), "dump"));
            roots.add(new File(v.dir, "Download"));
            roots.add(new File(v.dir, "Documents"));
            roots.add(v.dir);
        }
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
                    || n.equals(DataCheck.TITLE) && depth == 0)  // the game folder itself: its dump/ is searched first
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
    static String dumpProblem(Context c) {
        if (refreshOnly(c) || findDump(c) != null) return null;
        return Tr.f("no extracted game found. Extract the ExeFS and the RomFS of your Bayonetta 3 (update 1.2.0 "
                + "installed) with your emulator (Ryujinx: right-click > Extract Data > ExeFS, then RomFS; Eden / "
                + "yuzu: Dump RomFS > Full, and Dump ExeFS) and copy the folder with exefs and romfs to the phone, "
                + "for example into %s/dump or Download on the storage the game goes to (%s).", DataCheck.TITLE,
                where(c));
    }

    // "Internal storage" or the SD card's name: where the setup puts the game.
    static String where(Context c) {
        Storage.Volume v = Storage.volumeOf(c, DataCheck.target(c));
        return v != null ? v.name : DataCheck.target(c).getPath();
    }

    // The dump's size when it is on another volume than the game folder (the move is then a copy), else 0.
    static long crossVolumeBytes(Context c) {
        File dump = findDump(c);
        if (dump == null || Storage.sameVolume(c, dump, DataCheck.target(c))) return 0;
        return Storage.size(dump);
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
        File root = DataCheck.target(c);
        root.mkdirs();
        File log = new File(root, "setup.log");
        try (Writer w = new FileWriter(log, true)) {
            File b3 = new File(c.getFilesDir(), "b3");
            unpack(c, b3);
            String lib = c.getApplicationInfo().nativeLibraryDir;
            List<String> cmd = new ArrayList<>();
            cmd.add(lib + "/libb3python.so");
            cmd.add("-u");
            cmd.add(new File(b3, "repo/tools/b3_win.py").getPath());
            if (refreshOnly(c)) {  // set up before: bring it up to date with this version's tools and runtime
                cmd.add("update");
            } else {
                cmd.add("setup");
                cmd.add("--dump");
                cmd.add(dumpFolder(c).getPath());
            }
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
            env.put("B3_ANDROID_DATA", root.getPath());  // Internal storage/Bayonetta 3 or the SD card's
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
                if (ok) markBuilt(c);
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
