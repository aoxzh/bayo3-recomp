package io.github.bayonetta3recomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

// Keeps the on-phone setup (Setup.java) going while the player leaves the screen: a foreground service with a
// notification that shows the step and its progress, a Cancel action, and a partial wake lock so the CPU keeps
// compiling with the screen off. It only watches: the setup itself stays in Setup's thread and process.
public final class SetupService extends Service {
    private static final String CHANNEL = "setup";
    private static final int ID = 1;
    private static final String CANCEL = "io.github.bayonetta3recomp.CANCEL_SETUP";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private PowerManager.WakeLock wake;

    static void start(Context c) {
        try {
            c.startForegroundService(new Intent(c, SetupService.class));
        } catch (RuntimeException ignored) {  // not allowed from the background: the setup runs without it
        }
    }

    @Override public IBinder onBind(Intent i) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && CANCEL.equals(intent.getAction())) {
            Setup s = Setup.current();
            if (s != null) s.cancel();
            return START_NOT_STICKY;
        }
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Game setup", NotificationManager.IMPORTANCE_LOW));
        Notification n = notification("Starting", 0, 0, true);
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        else startForeground(ID, n);
        if (wake == null) {
            wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "b3:setup");
            wake.setReferenceCounted(false);
            wake.acquire(3 * 60 * 60 * 1000L);  // a setup takes under an hour; the lock never outlives three
        }
        ui.removeCallbacksAndMessages(null);
        ui.post(this::tick);
        return START_NOT_STICKY;
    }

    private void tick() {
        Setup s = Setup.current();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (s == null || !s.running) {
            boolean ok = s != null && s.ok;
            stopForeground(STOP_FOREGROUND_REMOVE);
            Notification.Builder b = base()
                    .setContentTitle(ok ? "The game is ready" : "Setup stopped")
                    .setContentText(ok ? "Open Bayonetta 3 Recomp and tap START."
                            : (s != null && s.error != null ? s.error : "Open the setup to carry on."))
                    .setAutoCancel(true);
            nm.notify(ID + 1, b.build());
            if (wake != null && wake.isHeld()) wake.release();
            stopSelf();
            return;
        }
        String phase;
        int done, total;
        synchronized (s) {
            phase = s.phase;
            done = s.done;
            total = s.total;
        }
        nm.notify(ID, notification(phase, done, total, false));
        ui.postDelayed(this::tick, 2000);
    }

    private Notification.Builder base() {
        Intent open = new Intent(this, SetupActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent tap = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentIntent(tap);
    }

    private Notification notification(String phase, int done, int total, boolean first) {
        Intent cancel = new Intent(this, SetupService.class).setAction(CANCEL);
        PendingIntent stop = PendingIntent.getService(this, 1, cancel,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder b = base()
                .setContentTitle("Setting up the game")
                .setContentText(total > 0 ? phase + " · " + done + " / " + total : phase)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(null, "Cancel", stop).build());
        if (total > 0) b.setProgress(total, Math.min(done, total), false);
        else b.setProgress(0, 0, !first);
        return b.build();
    }

    @Override public void onDestroy() {
        ui.removeCallbacksAndMessages(null);
        if (wake != null && wake.isHeld()) wake.release();
        super.onDestroy();
    }
}
