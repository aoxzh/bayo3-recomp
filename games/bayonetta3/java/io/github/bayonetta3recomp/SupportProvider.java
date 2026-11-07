package io.github.bayonetta3recomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

// Hands the support zip (<cache>/support/<name>.zip, LauncherActivity.shareReport) to the app picked in the share sheet,
// read-only and only through the URI permission the share intent grants. Nothing else is served.
public final class SupportProvider extends ContentProvider {
    static final String AUTHORITY = "io.github.bayonetta3recomp.support";

    static File dir(android.content.Context c) { return new File(c.getCacheDir(), "support"); }

    static Uri uri(File f) { return Uri.parse("content://" + AUTHORITY + "/" + Uri.encode(f.getName())); }

    private File file(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/") || !name.endsWith(".zip")) throw new FileNotFoundException(String.valueOf(uri));
        File f = new File(dir(getContext()), name);
        if (!f.isFile()) throw new FileNotFoundException(f.getPath());
        return f;
    }

    @Override public boolean onCreate() { return true; }

    @Override public String getType(Uri uri) { return "application/zip"; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read-only");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        File f;
        try {
            f = file(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] cols = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }

    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
