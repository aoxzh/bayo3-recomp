package io.github.bayonetta3recomp;

import android.content.Context;
import android.net.LocalServerSocket;
import android.net.LocalSocket;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

// The game built on a computer (tools/b3_win.py phone), handed to the app over USB: the computer's adb forwards a
// TCP port to this local socket and streams the game's library, which goes where the on-phone setup puts it
// (<files>/game/libbayonetta3.so, loaded by android/ondevice/loader.c). Only adb's shell user may connect, so no other
// app can hand the app code; the game's data (RomFS, shaders) goes to shared storage with adb push as before.
// Open while the launcher is in front (the computer starts it first).
//
// Protocol, one line each, UTF-8, ending in \n: the app sends "READY 1"; the computer sends
//   "B3-LIB 1 <bytes> <sha256 hex>" and that many bytes; the app answers "OK <bytes>" or "ERR <reason>". Or the
//   computer sends "WHERE 1"; the app answers "ROOT <path>": the game folder the data goes to (Settings > Game folder).
final class GameImport {
    static final String SOCKET = "io.github.bayonetta3recomp.import";
    private static final int SHELL_UID = 2000;
    private static final long MAX_BYTES = 2L << 30;

    interface Listener { void imported(String what); }

    private final Context context;
    private final Listener listener;
    private LocalServerSocket server;
    private Thread thread;

    GameImport(Context c, Listener l) { context = c.getApplicationContext(); listener = l; }

    synchronized void start() {
        if (server != null) return;
        try {
            server = new LocalServerSocket(SOCKET);
        } catch (IOException e) {
            server = null;  // another launcher instance holds it
            return;
        }
        final LocalServerSocket s = server;
        thread = new Thread(() -> serve(s), "b3-import");
        thread.setDaemon(true);
        thread.start();
    }

    synchronized void stop() {
        if (server == null) return;
        try {
            server.close();
        } catch (IOException ignored) {
        }
        server = null;
    }

    private void serve(LocalServerSocket s) {
        while (true) {
            LocalSocket c;
            try {
                c = s.accept();
            } catch (IOException e) {
                return;  // closed by stop()
            }
            try (LocalSocket peer = c) {
                if (peer.getPeerCredentials().getUid() != SHELL_UID) continue;
                handle(peer);
            } catch (IOException ignored) {
            }
        }
    }

    private void handle(LocalSocket peer) throws IOException {
        InputStream in = new BufferedInputStream(peer.getInputStream());
        OutputStream out = peer.getOutputStream();
        out.write("READY 1\n".getBytes(StandardCharsets.UTF_8));
        out.flush();
        String[] head = line(in).split(" ");
        String answer;
        if (head.length == 4 && head[0].equals("B3-LIB") && head[1].equals("1")) {
            answer = receiveLibrary(in, Long.parseLong(head[2]), head[3]);
        } else if (head.length == 2 && head[0].equals("WHERE") && head[1].equals("1")) {
            answer = "ROOT " + DataCheck.target(context).getAbsolutePath();  // Internal storage or the SD card
            out.write((answer + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            return;
        } else {
            answer = "ERR unknown request";
        }
        out.write((answer + "\n").getBytes(StandardCharsets.UTF_8));
        out.flush();
        if (answer.startsWith("OK") && listener != null) listener.imported("game");
    }

    private String receiveLibrary(InputStream in, long size, String sha) throws IOException {
        if (size <= 0 || size > MAX_BYTES) return "ERR size " + size;
        File lib = Setup.gameLibrary(context);
        File dir = lib.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) return "ERR cannot make " + dir;
        File part = new File(dir, lib.getName() + ".part");
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (Exception e) {
            return "ERR no SHA-256";
        }
        byte[] buf = new byte[1 << 20];
        long left = size;
        try (FileOutputStream f = new FileOutputStream(part)) {
            while (left > 0) {
                int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                if (n < 0) {
                    part.delete();
                    return "ERR short: " + (size - left) + " of " + size + " bytes";
                }
                f.write(buf, 0, n);
                digest.update(buf, 0, n);
                left -= n;
            }
            f.getFD().sync();
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format("%02x", b));
        if (!hex.toString().equalsIgnoreCase(sha)) {
            part.delete();
            return "ERR sha256 differs";
        }
        if (!part.renameTo(lib)) {
            part.delete();
            return "ERR cannot replace " + lib;
        }
        Setup.markBuilt(context);  // built on the computer for this phone: no SET UP again needed
        return "OK " + size;
    }

    private static String line(InputStream in) throws IOException {
        StringBuilder b = new StringBuilder();
        for (int c; (c = in.read()) != '\n'; ) {
            if (c < 0 || b.length() > 4096) throw new IOException("bad request line");
            b.append((char) c);
        }
        return b.toString();
    }
}
