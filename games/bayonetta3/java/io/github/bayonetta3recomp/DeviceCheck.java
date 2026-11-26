package io.github.bayonetta3recomp;

import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Environment;
import android.os.StatFs;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

// Whether this phone can run the game: below the minimum, the launcher says so before SET UP and START instead of
// letting a 30-60 minute setup end in a black screen (issues #9 and #11: a Mali-G57 MC2 whose driver rejects every
// pipeline, on a chip whose fastest cores run at 2.2 GHz). Measured to work: Adreno 740, 825 and 830 with cores at
// 3.2 GHz and up; an Adreno 825 with 7 GB of memory worked but swapped 2 GB. The desktop's b3_phone.py applies the
// same rules to a phone over USB (keep the two in step).
final class DeviceCheck {
    private DeviceCheck() {}

    static final String MINIMUM = Tr.t("Minimum: Vulkan 1.3, a CPU core at 2.4 GHz or faster, 6 GB of memory and an "
            + "Adreno 640 / Mali-G68 class GPU or newer. Recommended: Snapdragon 8 Gen 1 or newer with 8 GB.");

    static final class Result {
        final List<String> below = new ArrayList<>();   // under the minimum: SET UP and START stop and say why
        final List<String> weak = new ArrayList<>();    // works, but expect a low frame rate or stutters
        boolean ok() { return below.isEmpty(); }
        String key() { return String.join("|", below); }
    }

    static final long GB = 1000L * 1000 * 1000;

    // forSetup: also the free space the setup needs (the dump is moved, not copied; the build and shaders ~6 GB).
    static Result run(Context c, String gpu, boolean forSetup) {
        Result r = new Result();
        PackageManager pm = c.getPackageManager();
        if (!pm.hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION, 0x403000))
            r.below.add(Tr.t("No Vulkan 1.3 driver (the game draws with Vulkan 1.3)"));
        ActivityManager.MemoryInfo m = new ActivityManager.MemoryInfo();
        c.getSystemService(ActivityManager.class).getMemoryInfo(m);
        // totalMem is what the kernel manages: a "6 GB" phone reports about 5.5 GB.
        if (m.totalMem > 0 && m.totalMem < 5_300_000_000L)
            r.below.add(Tr.f("%.1f GB of memory (6 GB at least)", m.totalMem / (double) GB));
        else if (m.totalMem > 0 && m.totalMem < 7_300_000_000L)
            r.weak.add(Tr.f("%.1f GB of memory: the game may swap and stutter (8 GB recommended)",
                    m.totalMem / (double) GB));
        long khz = fastestCoreKhz();
        if (khz > 0 && khz < 2_400_000)
            r.below.add(Tr.f("Fastest CPU core %.1f GHz (2.4 GHz at least)", khz / 1e6));
        else if (khz > 0 && khz < 2_800_000)
            r.weak.add(Tr.f("Fastest CPU core %.1f GHz: expect under 30 fps in fights", khz / 1e6));
        if (gpu != null) gpuRule(gpu, r);
        if (forSetup) freeSpace(c, r);
        return r;
    }

    // The setup's work (the translated code, the objects) is in the app's own files on Internal storage: about 6 GB.
    // The game folder (Settings > Game folder) gets the shaders and code copies, about 1 GB, and the dump moved in: a
    // rename on the same storage, a copy taking the dump's size from another one (the SD card's dump into Internal
    // storage, or the other way).
    static void freeSpace(Context c, Result r) {
        Storage.Volume home = Storage.volumeOf(c, DataCheck.target(c));
        long dump = Setup.crossVolumeBytes(c);
        long internal = new StatFs(Environment.getExternalStorageDirectory().getPath()).getAvailableBytes();
        boolean apart = home != null && home.removable;
        long needInternal = 6 * GB + (apart ? 0 : dump);
        if (internal < needInternal)
            r.below.add(Tr.f(dump > 0 && !apart
                    ? "%.1f GB free in Internal storage (the setup needs about %.0f GB, with the dump copied in from "
                    + "the other storage)" : "%.1f GB free in Internal storage (the setup needs about %.0f GB)",
                    internal / (double) GB, needInternal / (double) GB));
        if (apart) {
            long free = home.free(), need = 1 * GB + dump;
            if (free < need)
                r.below.add(Tr.f(dump > 0
                        ? "%.1f GB free on %s (the game folder there needs about %.0f GB, with the dump copied in from "
                        + "Internal storage)" : "%.1f GB free on %s (the game folder there needs about %.0f GB)",
                        free / (double) GB, home.name, need / (double) GB));
        }
    }

    // The GPU from its GLES renderer name (ChipInfo.gpu()).
    static void gpuRule(String gpu, Result r) {
        Matcher a = Pattern.compile("Adreno\\s*(\\d+)").matcher(gpu);
        Matcher mali = Pattern.compile("Mali-G(\\d+)").matcher(gpu);
        if (a.find()) {
            int n = Integer.parseInt(a.group(1));
            if (n < 640) r.below.add(gpu + " " + Tr.t("(Adreno 640 or newer)"));
            else if (n < 730) r.weak.add(gpu + Tr.t(": a mid-range GPU, expect a low frame rate"));
        } else if (mali.find()) {
            int n = Integer.parseInt(mali.group(1));
            // Two digits: Bifrost and the first Valhall parts. G31-G76: entry-level or old drivers (the Mali-G57 MC2
            // of #9 could not build one pipeline); G68/G77/G78: mid-range. Three digits (G610 up), Immortalis: untested.
            if (n < 100 && n != 68 && n != 77 && n != 78)
                r.below.add(gpu + " " + Tr.t("(its driver could not build the game's shaders on a Mali-G57)"));
            else if (n < 100) r.weak.add(gpu + Tr.t(": a mid-range GPU, expect a low frame rate"));
            else r.weak.add(gpu + Tr.t(": not tested yet"));
        } else if (gpu.startsWith("Mali-T") || gpu.contains("PowerVR") || gpu.contains("Maleoon")) {
            r.below.add(gpu + " " + Tr.t("(too old for the game's shaders)"));
        } else {
            r.weak.add(gpu + Tr.t(": not tested yet"));
        }
    }

    static long fastestCoreKhz() {
        long best = 0;
        for (int i = 0; i < 16; i++) {
            File f = new File("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq");
            try {
                best = Math.max(best, Long.parseLong(new String(Files.readAllBytes(f.toPath()), StandardCharsets.US_ASCII).trim()));
            } catch (Exception ignored) {
            }
        }
        return best;
    }

    static String message(Result r) {
        StringBuilder b = new StringBuilder(Tr.t("The game is very unlikely to run on this phone:") + "\n");
        for (String s : r.below) b.append("\n• ").append(s);
        b.append("\n\n").append(MINIMUM).append("\n\n").append(Tr.t("A setup takes 30 to 60 minutes and about 6 GB, "
                + "so it is better not to start one here. You can still try: the game may not start, or may show only "
                + "a black screen."));
        return b.toString();
    }
}
