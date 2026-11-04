package com.roblox.client.startup;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Environment;
import android.os.Build;
import android.util.Log;
import android.widget.Toast;
import java.io.File;
import java.lang.ref.WeakReference;

/** Explain shared workspace access once; Android remains responsible for granting it. */
public final class BasaltWorkspaceAccess implements DialogInterface.OnClickListener, Runnable {
    private static final String SEEN = "workspace_access_prompted_v1";
    private static AlertDialog current;
    private final WeakReference<Activity> owner;

    private BasaltWorkspaceAccess(Activity activity) { owner = new WeakReference<>(activity); }

    private static boolean allowed() {
        try { return (Boolean)Environment.class.getMethod("isExternalStorageManager").invoke(null); }
        catch (ReflectiveOperationException error) { return false; }
    }

    public static boolean showOnce(Activity activity) {
        if (activity.isFinishing()) return true;
        BasaltInputSimulation.attach(activity);
        // Android 10 uses the app workspace and has no all-files settings page.
        if (Build.VERSION.SDK_INT < 30) return false;
        if (current != null) {
            if (current.getOwnerActivity() == activity && current.isShowing()) return true;
            current.dismiss();
            current = null;
        }
        if (allowed()) {
            File directory = new File(Environment.getExternalStorageDirectory(), "Basalt/workspace");
            if (!directory.isDirectory() && !directory.mkdirs()) Log.w("BasaltStorage", "Shared workspace could not be created");
            return false;
        }
        SharedPreferences prefs = activity.getSharedPreferences("basalt_startup", 0);
        if (prefs.getBoolean(SEEN, false)) return false;
        BasaltWorkspaceAccess callback = new BasaltWorkspaceAccess(activity);
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle("Basalt workspace")
            .setMessage("Allow file access to save scripts in Basalt/workspace, where your file manager can reach them.\n\nOn the next screen, enable Allow access to manage all files. You can choose Not now to keep using app-only storage.")
            .setPositiveButton("Allow access", callback)
            .setNegativeButton("Not now", callback)
            .setCancelable(false).create();
        try {
            dialog.setOwnerActivity(activity);
            dialog.show();
            current = dialog;
        } catch (android.view.WindowManager.BadTokenException error) {
            Log.w("BasaltStorage", "Waiting for a usable startup window");
        }
        return true;
    }

    @Override public void onClick(DialogInterface dialog, int which) {
        Activity activity = owner.get();
        current = null;
        if (activity == null || activity.isFinishing()) return;
        if (which == DialogInterface.BUTTON_POSITIVE) {
            try {
                activity.startActivity(new Intent("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION",
                    Uri.parse("package:" + activity.getPackageName())));
            } catch (ActivityNotFoundException | SecurityException error) {
                Toast.makeText(activity, "Open Android Settings to allow file access for Roblox.", Toast.LENGTH_LONG).show();
                return;
            }
        }
        activity.getSharedPreferences("basalt_startup", 0).edit().putBoolean(SEEN, true).commit();
        if (which != DialogInterface.BUTTON_POSITIVE) activity.getWindow().getDecorView().post(this);
    }

    @Override public void run() {
        Activity activity = owner.get();
        if (activity == null || activity.isFinishing()) return;
        try {
            Class.forName("com.roblox.client.startup.BasaltUpdates")
                .getMethod("showOnce", Activity.class).invoke(null, activity);
        } catch (ReflectiveOperationException error) {
            Log.w("BasaltStorage", "Could not continue startup prompts", error);
        }
    }
}
