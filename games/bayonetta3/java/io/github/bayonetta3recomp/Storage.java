package io.github.bayonetta3recomp;

import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.os.StatFs;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.Writer;
import java.util.ArrayList;
import java.util.List;

// Where the game folder can live: Internal storage, or a mounted SD card (or USB drive) when the device has one. The
// choice is <files>/data_root.txt, which runtime/src/platform_android.cpp reads first; no file means Internal storage/
// Bayonetta 3. All-files access covers removable volumes too, so nothing else is needed to read or write there.
// Only the data moves: the built game (<files>/game/libbayonetta3.so) and the setup's work stay in the app's own files,
// since an SD card is usually exFAT or FAT32 and holds no executables or links.
final class Storage {
    static final class Volume {
        final File dir;        // the volume's top, /storage/emulated/0 or /storage/1234-ABCD
        final String name;     // "Internal storage", "SD card", or what the system calls it
        final boolean removable;
        Volume(File dir, String name, boolean removable) { this.dir = dir; this.name = name; this.removable = removable; }
        File gameFolder() { return new File(dir, DataCheck.TITLE); }
        long free() {
            try {
                return new StatFs(dir.getPath()).getAvailableBytes();
            } catch (IllegalArgumentException e) {
                return 0;
            }
        }
    }

    static File choiceFile(Context c) { return new File(c.getFilesDir(), "data_root.txt"); }

    // Internal storage first, then every mounted removable volume.
    static List<Volume> volumes(Context c) {
        List<Volume> out = new ArrayList<>();
        out.add(new Volume(Environment.getExternalStorageDirectory(), "Internal storage", false));
        if (Build.VERSION.SDK_INT < 30) return out;  // StorageVolume.getDirectory; all-files access is 30+ anyway
        StorageManager sm = c.getSystemService(StorageManager.class);
        if (sm == null) return out;
        for (StorageVolume v : sm.getStorageVolumes()) {
            if (v.isPrimary() || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            File d = v.getDirectory();
            if (d == null || !d.isDirectory()) continue;
            String name = v.getDescription(c);
            out.add(new Volume(d, name == null || name.isEmpty() ? "SD card" : name, v.isRemovable()));
        }
        return out;
    }

    // The volume a path is on (the longest top that holds it), or null.
    static Volume volumeOf(Context c, File f) {
        if (f == null) return null;
        String p = f.getAbsolutePath() + "/";
        Volume best = null;
        for (Volume v : volumes(c)) {
            String top = v.dir.getAbsolutePath() + "/";
            if (p.startsWith(top) && (best == null || top.length() > best.dir.getAbsolutePath().length() + 1)) best = v;
        }
        return best;
    }

    static boolean sameVolume(Context c, File a, File b) {
        Volume x = volumeOf(c, a), y = volumeOf(c, b);
        return x != null && y != null && x.dir.equals(y.dir);
    }

    // The game folder on `v`: Internal storage is the default (the choice file is removed), any other is written.
    static void choose(Context c, Volume v) throws IOException {
        File f = choiceFile(c);
        if (!v.removable && v.dir.equals(Environment.getExternalStorageDirectory())) {
            if (f.exists() && !f.delete()) throw new IOException("could not remove " + f);
            return;
        }
        try (Writer w = new FileWriter(f)) { w.write(v.gameFolder().getAbsolutePath() + "\n"); }
    }

    static long size(File f) {
        if (java.nio.file.Files.isSymbolicLink(f.toPath())) return 0;
        if (f.isFile()) return f.length();
        long n = 0;
        File[] in = f.listFiles();
        if (in != null) for (File x : in) n += size(x);
        return n;
    }

    // Moving the game folder to another volume: everything copied first (the old folder stays whole until every file
    // arrived with its size), then the choice switched, then the old folder deleted. Stopped half way, the old folder
    // is still the game and the partial copy is cleared on the next try.
    static final class Move {
        interface Progress { void at(long done, long total, String file); }

        final File from, to;
        final long total;
        volatile boolean cancelled;

        Move(File from, File to) {
            this.from = from;
            this.to = to;
            this.total = size(from);
        }

        void run(Context c, Volume target, Progress p) throws IOException {
            File part = new File(to.getParentFile(), to.getName() + ".partial");
            deleteTree(part);
            String[] there = to.list();
            if (there != null && there.length == 0) to.delete();  // an empty folder is fine; anything in it is kept
            if (to.exists())
                throw new IOException(to + " already exists (a dump in it?): move what is in it elsewhere or rename "
                        + "it first");
            long[] done = {0};
            byte[] buf = new byte[1 << 20];
            try {
                copyTree(from, part, buf, done, p);
                if (size(part) != total) throw new IOException("the copy is incomplete: try again");
            } catch (IOException e) {
                deleteTree(part);  // the old folder is untouched; no half copy left taking the space
                throw e;
            }
            if (!part.renameTo(to)) throw new IOException("could not rename " + part + " to " + to);
            choose(c, target);
            deleteTree(from);
        }

        private void copyTree(File src, File dst, byte[] buf, long[] done, Progress p) throws IOException {
            if (cancelled) throw new IOException("stopped");
            if (java.nio.file.Files.isSymbolicLink(src.toPath())) return;
            if (src.isDirectory()) {
                if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("could not create " + dst);
                File[] in = src.listFiles();
                if (in != null) for (File x : in) copyTree(x, new File(dst, x.getName()), buf, done, p);
                return;
            }
            // FAT32 holds no file of 4 GB or more; the game's archives are smaller, but say so plainly if one is not.
            try (InputStream i = new FileInputStream(src); OutputStream o = new FileOutputStream(dst)) {
                int k;
                long since = 0;
                while ((k = i.read(buf)) > 0) {
                    o.write(buf, 0, k);
                    done[0] += k;
                    since += k;
                    if (since >= 32 << 20) {
                        since = 0;
                        p.at(done[0], total, src.getName());
                        if (cancelled) throw new IOException("stopped");
                    }
                }
            } catch (IOException e) {
                if (src.length() >= 4L << 30)
                    throw new IOException(src.getName() + " is 4 GB or more, which a FAT32 card cannot hold: format the "
                            + "card as exFAT", e);
                throw e;
            }
            p.at(done[0], total, src.getName());
        }
    }

    static void deleteTree(File f) {
        File[] in = f.listFiles();
        if (in != null && !java.nio.file.Files.isSymbolicLink(f.toPath())) for (File x : in) deleteTree(x);
        f.delete();
    }

    private Storage() {}
}
