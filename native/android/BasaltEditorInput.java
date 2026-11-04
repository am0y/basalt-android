package com.roblox.client.startup;

import android.os.Handler;
import android.os.Looper;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.text.Layout;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.FrameLayout;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Keep the Android input viewport and Roblox's code canvas in the same coordinates. */
public final class BasaltEditorInput implements Runnable {
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final BasaltEditorInput APPLY = new BasaltEditorInput();
    private static volatile float[] viewport = new float[7];
    private static volatile float[] snapshot = new float[23];
    private static volatile int requested;
    private static int applied;
    private static volatile float[] scrollTarget = new float[2];
    private static volatile int scrollRequested;
    private static int scrollApplied;
    private static float reportedX = -1, reportedY = -1;
    private static WeakReference<EditText> active = new WeakReference<>(null);
    private static long textBox;
    private static volatile float density = 1;
    private static boolean code, classifierDisabled, applying;
    private static Object originalClassifier;
    private static Method classifierSetter;
    private static int[] padding;
    private static int left = Integer.MIN_VALUE, top, width, height;
    private static final Rect clip = new Rect();
    private static Method clipSetter;
    private static Method fallbackSpacingSetter;
    private static boolean originalFallbackSpacing;
    private static float[] applyingViewport;

    private BasaltEditorInput() {}

    public static synchronized float[] exchange(float x, float y, float w, float h, float originX, float originY, boolean enabled) {
        float[] next = {x, y, w, h, originX, originY, enabled ? 1 : 0};
        if (!Arrays.equals(next, viewport)) {
            float sx = Math.max(0, x - originX), sy = Math.max(0, y - originY);
            boolean moved = Math.abs(sx - (viewport[0] - viewport[4])) > 0.01f ||
                Math.abs(sy - (viewport[1] - viewport[5])) > 0.01f;
            // A native scroll is mirrored back by Lua on its next frame. Acknowledge
            // that echo without rewinding newer touch movement to the older snapshot.
            boolean echo = Math.abs(sx - reportedX) <= 1 / density && Math.abs(sy - reportedY) <= 1 / density;
            if ((moved && !echo) || enabled && viewport[6] == 0) {
                scrollTarget = new float[]{sx, sy};
                scrollRequested++;
            }
            viewport = next;
            requested++;
            UI.removeCallbacks(APPLY);
            UI.post(APPLY);
        }
        float[] result = snapshot.clone();
        if (result[1] > 0) {
            reportedX = result[2] / result[1];
            reportedY = result[3] / result[1];
        }
        result[8] = requested;
        return result;
    }

    public static void configure(EditText view, boolean isCode, float scale, long id) {
        if (!isCode || id != textBox && viewport[6] == 0) {
            if (active.get() == view) detach(view);
            return;
        }
        boolean changed = active.get() != view || textBox != id || density != scale;
        if (active.get() != view || textBox != id) {
            EditText old = active.get();
            if (old != null) detach(old);
            active = new WeakReference<>(view);
            textBox = id;
            synchronized (BasaltEditorInput.class) {
                scrollTarget = new float[]{Math.max(0, viewport[0] - viewport[4]), Math.max(0, viewport[1] - viewport[5])};
                scrollRequested++;
            }
            padding = new int[]{view.getPaddingLeft(), view.getPaddingTop(), view.getPaddingRight(), view.getPaddingBottom()};
            try { clipSetter = View.class.getMethod("setClipBounds", Rect.class); }
            catch (ReflectiveOperationException ignored) { clipSetter = null; }
            try {
                fallbackSpacingSetter = view.getClass().getMethod("setFallbackLineSpacing", boolean.class);
                originalFallbackSpacing = (Boolean)view.getClass().getMethod("isFallbackLineSpacing").invoke(view);
                fallbackSpacingSetter.invoke(view, false);
            } catch (ReflectiveOperationException ignored) { fallbackSpacingSetter = null; }
            // NO_OP retains ordinary selection/copy/paste, without asynchronous smart-select animation.
            try {
                Class<?> type = Class.forName("android.view.textclassifier.TextClassifier");
                originalClassifier = view.getClass().getMethod("basaltOriginalTextClassifier").invoke(view);
                classifierSetter = view.getClass().getMethod("setTextClassifier", type);
                classifierSetter.invoke(view, type.getField("NO_OP").get(null));
                classifierDisabled = view.getClass().getMethod("getTextClassifier").invoke(view) == type.getField("NO_OP").get(null);
            } catch (ReflectiveOperationException ignored) {
                classifierDisabled = false;
            }
        }
        code = true;
        density = scale > 0 ? scale : 1;
        view.setPadding(0, 0, 0, 0);
        view.setHorizontallyScrolling(true);
        if (changed || applied != requested) APPLY.run();
        else record(view);
    }

    public static void detach(EditText view) {
        if (active.get() != view) return;
        UI.removeCallbacks(APPLY);
        // Cancel selection while its layout and code classifier are still intact.
        view.cancelLongPress();
        view.clearFocus();
        if (classifierSetter != null && originalClassifier != null) {
            try { classifierSetter.invoke(view, originalClassifier); } catch (ReflectiveOperationException ignored) {}
        }
        if (padding != null) view.setPadding(padding[0], padding[1], padding[2], padding[3]);
        if (clipSetter != null) {
            try { clipSetter.invoke(view, new Object[]{null}); } catch (ReflectiveOperationException ignored) {}
        }
        if (fallbackSpacingSetter != null) {
            try { fallbackSpacingSetter.invoke(view, originalFallbackSpacing); } catch (ReflectiveOperationException ignored) {}
            fallbackSpacingSetter = null;
        }
        view.setHorizontallyScrolling((view.getInputType() & 0x20000) == 0);
        originalClassifier = null;
        classifierSetter = null;
        padding = null;
        classifierDisabled = code = false;
        textBox = 0;
        left = Integer.MIN_VALUE;
        active.clear();
        snapshot = new float[23];
    }

    public static boolean prepareLayout(EditText view, ViewGroup.LayoutParams params) {
        if (!code || active.get() != view || !(params instanceof FrameLayout.LayoutParams)) return true;
        FrameLayout.LayoutParams p = (FrameLayout.LayoutParams) params;
        boundLayout(view, p, applying && applyingViewport != null ? applyingViewport : viewport);
        boolean changed = p.leftMargin != left || p.topMargin != top || p.width != width || p.height != height;
        left = p.leftMargin; top = p.topMargin; width = p.width; height = p.height;
        if (!applying && changed) {
            UI.removeCallbacks(APPLY);
            UI.post(APPLY);
        }
        return changed;
    }

    private static void boundLayout(EditText view, FrameLayout.LayoutParams p, float[] box) {
        int[] parent = new int[2];
        if (view.getParent() instanceof View) ((View)view.getParent()).getLocationOnScreen(parent);
        p.leftMargin = Math.round(box[0] * density) - parent[0];
        p.topMargin = Math.round(box[1] * density) - parent[1];
        p.width = box[6] != 0 ? Math.max(1, Math.round(box[2] * density)) : 1;
        p.height = box[6] != 0 ? Math.max(1, Math.round(box[3] * density)) : 1;
    }

    @Override public void run() {
        applyViewport();
    }

    private static void applyViewport() {
        EditText view = active.get();
        if (!code || view == null || applying) return;
        applying = true;
        try {
            final float[] box, target;
            final int revision, scrollRevision;
            // Never hold the JNI exchange monitor across Android View callbacks:
            // they may synchronously enter the engine that is publishing a viewport.
            synchronized (BasaltEditorInput.class) {
                box = viewport;
                target = scrollTarget;
                revision = requested;
                scrollRevision = scrollRequested;
            }
            applyingViewport = box;
            if (view.getLayoutParams() instanceof FrameLayout.LayoutParams) {
                FrameLayout.LayoutParams p = (FrameLayout.LayoutParams)view.getLayoutParams();
                boundLayout(view, p, box);
                view.setLayoutParams(p);
            }
            if (box[6] != 0) clip.set(0, 0, Math.max(0, width), Math.max(0, height));
            else clip.setEmpty();
            if (clipSetter != null) {
                try { clipSetter.invoke(view, clip); } catch (ReflectiveOperationException ignored) {}
            }
            if (scrollApplied != scrollRevision) {
                int x = Math.max(0, Math.round(target[0] * density));
                int y = Math.max(0, Math.round(target[1] * density));
                if (view.getScrollX() != x || view.getScrollY() != y) view.scrollTo(x, y);
                scrollApplied = scrollRevision;
            }
            view.invalidate();
            applied = revision;
        } finally { applyingViewport = null; applying = false; }
        record(view);
    }

    public static void afterLayout(EditText view) {
        if (active.get() != view || !code) return;
        APPLY.run();
    }

    public static boolean ownsScroll(EditText view) {
        // Roblox reveals the caret after edits. TextView's deferred auto-pan can otherwise
        // jump to an old caret after every native geometry notification.
        return code && active.get() == view;
    }

    public static boolean acceptsTouch(EditText view, MotionEvent event) {
        return !code || active.get() != view || event.getActionMasked() != MotionEvent.ACTION_DOWN ||
            clip.contains((int)event.getX(), (int)event.getY());
    }

    public static void clipDraw(EditText view, Canvas canvas) {
        if (code && active.get() == view) canvas.clipRect(clip.left + view.getScrollX(), clip.top + view.getScrollY(),
            clip.right + view.getScrollX(), clip.bottom + view.getScrollY());
    }

    public static void record(EditText view) {
        if (active.get() != view || !code || applying) return;
        int[] location = new int[2];
        view.getLocationOnScreen(location);
        Layout layout = view.getLayout();
        int start = Math.max(0, view.getSelectionStart());
        int line = layout != null ? layout.getLineForOffset(Math.min(start, view.length())) : 0;
        int lines = layout != null ? layout.getLineCount() : 0;
        snapshot = new float[]{view.hasFocus() && view.getVisibility() == View.VISIBLE && viewport[6] != 0 ? 1 : 0,
            density, view.getScrollX(), view.getScrollY(), view.getLineHeight(),
            view.getSelectionStart(), view.getSelectionEnd(), applied, requested, classifierDisabled ? 1 : 0,
            location[0] + clip.left, location[1] + clip.top, clip.width(), clip.height(),
            line, layout != null ? layout.getLineTop(line) : 0,
            layout != null ? layout.getLineBottom(line) : 0, lines,
            layout != null ? layout.getLineTop(lines) : 0,
            layout != null ? layout.getPrimaryHorizontal(Math.min(start, view.length())) : 0,
            utf8Offset(view.getText(), view.getSelectionStart()), utf8Offset(view.getText(), view.getSelectionEnd()),
            fallbackSpacingSetter != null ? 1 : 0};
    }

    private static int utf8Offset(CharSequence text, int offset) {
        if (offset < 0) return -1;
        int bytes = 0;
        for (int i = 0; i < Math.min(offset, text.length()); i++) {
            char c = text.charAt(i);
            if (c < 0x80) bytes++;
            else if (c < 0x800) bytes += 2;
            else if (Character.isHighSurrogate(c) && i + 1 < offset && Character.isLowSurrogate(text.charAt(i + 1))) { bytes += 4; i++; }
            else bytes += 3;
        }
        return bytes;
    }
}
