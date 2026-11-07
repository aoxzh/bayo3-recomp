package io.github.bayonetta3recomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

// <data root>/config.txt, the KEY=VALUE file runtime/src/platform_android.cpp reads at startup: NX_* lines become
// environment variables (the first line for a key wins), `language` is read with plat::setting. The launcher owns the
// keys in MANAGED; every other line (test switches written by scripts, comments) is kept as it is, in its place.
final class Config {
    static final String[] MANAGED = {"NX_PROFILE", "NX_FPS_CAP", "NX_DISPLAY_HZ", "language", "NX_PAD_SWAP", "NX_MUTE",
            "NX_TOUCH_SHOW", "NX_TOUCH_OPACITY", "NX_TOUCH_SIZE"};
    static final Set<String> MANAGED_SET = new HashSet<>(Arrays.asList(MANAGED));
    private static final String HEADER = "# set by the launcher (profile and settings):";

    final File file;
    final Map<String, String> values = new LinkedHashMap<>();
    final List<String> other = new ArrayList<>();

    private Config(File file) { this.file = file; }

    static Config load(File file) {
        Config c = new Config(file);
        if (!file.isFile()) return c;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) c.take(line);
        } catch (IOException ignored) {
        }
        while (!c.other.isEmpty() && c.other.get(c.other.size() - 1).trim().isEmpty()) c.other.remove(c.other.size() - 1);
        return c;
    }

    private void take(String line) {
        String trimmed = line.replaceAll("[\\r ]+$", "");
        if (trimmed.equals(HEADER)) return;
        String key = key(trimmed);
        if (key != null && MANAGED_SET.contains(key)) {
            if (!values.containsKey(key)) values.put(key, trimmed.substring(key.length() + 1));
            return;
        }
        other.add(line);
    }

    private static String key(String line) {
        int eq = line.indexOf('=');
        return eq > 0 && !line.startsWith("#") ? line.substring(0, eq) : null;
    }

    String get(String key, String fallback) {
        String v = values.get(key);
        return v == null || v.isEmpty() ? fallback : v;
    }

    void set(String key, String value) {
        if (value == null) values.remove(key);
        else values.put(key, value);
    }

    String otherText() {
        StringBuilder b = new StringBuilder();
        for (String l : other) b.append(l).append('\n');
        return b.toString();
    }

    // The Advanced box's text. A line for a key the launcher manages is dropped: the setting above it decides.
    void setOtherText(String text) {
        other.clear();
        for (String l : text.split("\n", -1)) {
            String key = key(l.trim());
            if (key != null && MANAGED_SET.contains(key)) continue;
            other.add(l.replaceAll("\\r$", ""));
        }
        while (!other.isEmpty() && other.get(other.size() - 1).trim().isEmpty()) other.remove(other.size() - 1);
    }

    void save() throws IOException {
        File dir = file.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        File tmp = new File(dir, file.getName() + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) {
            for (String l : other) w.write(l + "\n");
            w.write(HEADER + "\n");
            for (String k : MANAGED) {
                String v = values.get(k);
                if (v != null) w.write(k + "=" + v + "\n");
            }
        }
        if (!tmp.renameTo(file)) {
            file.delete();
            if (!tmp.renameTo(file)) throw new IOException("cannot replace " + file);
        }
    }

    // ---- the performance profiles (runtime/src/nvn/nvn_queue.cpp NX_FPS_CAP, runtime/src/hle_vi.cpp NX_DISPLAY_HZ) ----

    static final String SAVER = "saver", BALANCED = "balanced", UNLOCKED = "unlocked";

    String profile() {
        String cap = get("NX_FPS_CAP", "30");  // unset: the Android default, 30 (platform_android.cpp)
        if (cap.equals("30")) return SAVER;
        return "40".equals(values.get("NX_DISPLAY_HZ")) ? BALANCED : UNLOCKED;
    }

    void setProfile(String p) {
        set("NX_PROFILE", p);
        set("NX_FPS_CAP", p.equals(SAVER) ? "30" : "60");
        set("NX_DISPLAY_HZ", p.equals(BALANCED) ? "40" : null);
    }
}
