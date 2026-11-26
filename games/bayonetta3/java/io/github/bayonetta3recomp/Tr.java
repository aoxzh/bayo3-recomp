package io.github.bayonetta3recomp;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

// The app's text in the phone's language: English (as written in the code), Simplified Chinese or Japanese. The English
// text is the key: t("Settings") is "设置" on a Chinese phone, "設定" on a Japanese one, and "Settings" on any other
// (also for a text with no translation yet). f() is the same for a String.format pattern. The table is TrText.java;
// tests/test_launcher_i18n.py checks that every t("...") and f("...") in the app has its line there.
final class Tr {
    private Tr() {}

    private static final int ZH = 1, JA = 2;
    private static int lang = -1;
    private static Map<String, String[]> table;

    private static synchronized int lang() {
        if (lang < 0) {
            String l = Locale.getDefault().getLanguage();
            lang = "zh".equals(l) ? ZH : "ja".equals(l) ? JA : 0;
            table = new HashMap<>();
            for (String[] row : TrText.ROWS) table.put(row[0], row);
        }
        return lang;
    }

    static boolean chinese() { return lang() == ZH; }

    static String t(String en) {
        int l = lang();
        if (l == 0) return en;
        String[] row = table.get(en);
        return row == null || row[l] == null || row[l].isEmpty() ? en : row[l];
    }

    static String f(String en, Object... args) { return String.format(Locale.ROOT, t(en), args); }
}
