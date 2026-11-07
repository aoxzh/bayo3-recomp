package io.github.bayonetta3recomp;

import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;

// The hardware as chips: the SoC (Build.SOC_MODEL) and the GPU (the GLES renderer string). Never the device's model
// name: reports and screenshots name the chip, which is what decides performance.
final class ChipInfo {
    private ChipInfo() {}

    // Marketing names for the Snapdragon parts the port has been measured on or is likely to meet.
    private static String snapdragon(String model) {
        switch (model) {
            case "SM8450": return "Snapdragon 8 Gen 1";
            case "SM8475": return "Snapdragon 8+ Gen 1";
            case "SM8550": return "Snapdragon 8 Gen 2";
            case "SM8635": return "Snapdragon 8s Gen 3";
            case "SM8650": return "Snapdragon 8 Gen 3";
            case "SM8750": return "Snapdragon 8 Elite";
            case "SM8850": return "Snapdragon 8 Elite Gen 5";
            case "SM7675": return "Snapdragon 7+ Gen 3";
            case "SM7550": return "Snapdragon 7 Gen 3";
            case "SM6450": return "Snapdragon 6 Gen 1";
            case "QCS8550": return "Snapdragon 8 Gen 2 (QCS8550)";
            default: return null;
        }
    }

    static String soc() {
        if (Build.VERSION.SDK_INT >= 31) {
            String model = Build.SOC_MODEL, maker = Build.SOC_MANUFACTURER;
            if (model != null && !model.isEmpty() && !model.equals(Build.UNKNOWN)) {
                String name = snapdragon(model);
                if (name != null) return name.contains("(") ? name : name + " (" + model + ")";
                return (maker == null || maker.equals(Build.UNKNOWN) ? "" : maker + " ") + model;
            }
        }
        return Build.HARDWARE;
    }

    // "Adreno (TM) 830" -> "Adreno 830". Runs a throwaway 1x1 pbuffer context; call off the UI thread.
    static String gpu() {
        EGLDisplay dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (dpy == EGL14.EGL_NO_DISPLAY) return null;
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(dpy, ver, 0, ver, 1)) return null;
        String renderer = null;
        try {
            int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                    EGL14.EGL_NONE};
            EGLConfig[] cfg = new EGLConfig[1];
            int[] n = new int[1];
            if (!EGL14.eglChooseConfig(dpy, attrs, 0, cfg, 0, 1, n, 0) || n[0] == 0) return null;
            EGLContext ctx = EGL14.eglCreateContext(dpy, cfg[0], EGL14.EGL_NO_CONTEXT,
                    new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            EGLSurface surf = EGL14.eglCreatePbufferSurface(dpy, cfg[0], new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1,
                    EGL14.EGL_NONE}, 0);
            if (EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) renderer = GLES20.glGetString(GLES20.GL_RENDERER);
            EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(dpy, surf);
            EGL14.eglDestroyContext(dpy, ctx);
        } finally {
            EGL14.eglTerminate(dpy);
        }
        return renderer == null ? null : renderer.replace("(TM) ", "").replace("(TM)", "").trim();
    }
}
