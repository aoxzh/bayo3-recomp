package io.github.bayonetta3recomp;

import android.content.Context;
import android.os.Environment;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// The data folder and what the game needs in it, the same choice android_main makes (runtime/src/platform_android.cpp):
// the folder picked earlier (<files>/data_root.txt) if it holds the data, else the app's own external files folder if it
// does, else Internal storage/Bayonetta 3.
final class DataCheck {
    static final String TITLE = "Bayonetta 3";

    static final class Item {
        final String what, path, why;
        Item(String what, String path, String why) { this.what = what; this.path = path; this.why = why; }
    }

    File root;
    boolean shared;          // Internal storage/Bayonetta 3 (needs all-files access)
    boolean folderExists;
    final List<Item> missing = new ArrayList<>();
    long cacheBytes;         // pipelines_<vendor>_<device>.bin, nvn_pipeline_cache.cpp
    int cacheFiles;

    boolean ready() { return folderExists && missing.isEmpty(); }

    static File sharedRoot() { return new File(Environment.getExternalStorageDirectory(), TITLE); }

    static boolean hasData(File root) { return root != null && new File(root, "analysis/main.bin").canRead(); }

    static File ownRoot(Context c) { return c.getExternalFilesDir(null); }

    static File chooseRoot(Context c) {
        File chosen = null;
        try (BufferedReader r = new BufferedReader(new FileReader(new File(c.getFilesDir(), "data_root.txt")))) {
            String line = r.readLine();
            if (line != null && !line.trim().isEmpty()) chosen = new File(line.trim());
        } catch (Exception ignored) {
        }
        if (chosen != null && hasData(chosen)) return chosen;
        File own = ownRoot(c);
        if (hasData(own)) return own;
        return sharedRoot();
    }

    static DataCheck run(Context c) {
        DataCheck d = new DataCheck();
        d.root = chooseRoot(c);
        d.shared = d.root.equals(sharedRoot());
        d.folderExists = d.root.isDirectory();
        if (!d.folderExists) return d;
        for (String m : new String[] {"main", "subsdk0", "sdk"}) {
            if (!new File(d.root, "analysis/" + m + ".bin").canRead())
                d.missing.add(new Item("Game code", "analysis/" + m + ".bin", "made from the game's ExeFS by the setup tools"));
        }
        String[] romfs = new File(d.root, "extract/romfs").list();
        if (romfs == null || romfs.length == 0)
            d.missing.add(new Item("RomFS", "extract/romfs/", "the game's RomFS, extracted from your dump"));
        if (!new File(d.root, "spirv/results.tsv").canRead())
            d.missing.add(new Item("Shaders", "spirv/", "the translated shaders: without them nothing is drawn"));
        File[] caches = d.root.listFiles((dir, name) -> name.startsWith("pipelines_") && name.endsWith(".bin"));
        if (caches != null) {
            for (File f : caches) {
                d.cacheBytes += f.length();
                d.cacheFiles++;
            }
        }
        return d;
    }

    // "Internal storage/Bayonetta 3" for the shared folder, else the path.
    String rootLabel() {
        return shared ? "Internal storage/" + TITLE : root.getAbsolutePath();
    }

    static String size(long bytes) {
        if (bytes >= 1L << 30) return String.format(Locale.US, "%.1f GB", bytes / (double) (1L << 30));
        if (bytes >= 1L << 20) return String.format(Locale.US, "%.1f MB", bytes / (double) (1L << 20));
        return String.format(Locale.US, "%d KB", Math.max(1, bytes >> 10));
    }
}
