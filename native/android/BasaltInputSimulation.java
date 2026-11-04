package com.roblox.client.startup;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.LinkedHashMap;
import java.util.Map;

/** Send executor input to Roblox's own render surface on Android's UI thread. */
public final class BasaltInputSimulation {
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static volatile WeakReference<Activity> owner = new WeakReference<>(null);
    private static final LinkedHashMap<Integer, float[]> touches = new LinkedHashMap<>();
    private static float mouseX = -1, mouseY = -1;
    private static int buttons;
    private static long downTime;

    private BasaltInputSimulation() {}

    public static void attach(Activity activity) { owner = new WeakReference<>(activity); }

    public static boolean foreground() {
        Activity activity = owner.get();
        return activity != null && !activity.isFinishing() && activity.hasWindowFocus();
    }

    // kind: key down/up, mouse absolute/relative/press/release/scroll/click,
    //       touch down/move/up/click. All coordinates are Roblox viewport pixels.
    public static boolean send(int kind, int code, float x, float y, float vw, float vh) {
        if (kind < 0 || kind > 11 || !Float.isFinite(x) || !Float.isFinite(y) ||
            !Float.isFinite(vw) || !Float.isFinite(vh) || vw <= 0 || vh <= 0 || !foreground()) return false;
        if (kind <= 1 && key(code) < 0) return false;
        if (kind >= 4 && kind <= 7 && kind != 6 && code != 1 && code != 2 && code != 4) return false;
        if (kind >= 8 && (code < 0 || code > 9)) return false;
        UI.post(new Event(kind, code, x, y, vw, vh));
        return true;
    }

    private static int key(int code) {
        if (code >= 65 && code <= 90) return KeyEvent.KEYCODE_A + code - 65;
        if (code >= 97 && code <= 122) return KeyEvent.KEYCODE_A + code - 97;
        if (code >= 48 && code <= 57) return KeyEvent.KEYCODE_0 + code - 48;
        switch (code) {
            case 8: return KeyEvent.KEYCODE_DEL;
            case 9: return KeyEvent.KEYCODE_TAB;
            case 13: return KeyEvent.KEYCODE_ENTER;
            case 27: return KeyEvent.KEYCODE_ESCAPE;
            case 32: return KeyEvent.KEYCODE_SPACE;
            case 273: return KeyEvent.KEYCODE_DPAD_UP;
            case 274: return KeyEvent.KEYCODE_DPAD_DOWN;
            case 275: return KeyEvent.KEYCODE_DPAD_RIGHT;
            case 276: return KeyEvent.KEYCODE_DPAD_LEFT;
            case 303: return KeyEvent.KEYCODE_SHIFT_RIGHT;
            case 304: return KeyEvent.KEYCODE_SHIFT_LEFT;
            case 305: return KeyEvent.KEYCODE_CTRL_RIGHT;
            case 306: return KeyEvent.KEYCODE_CTRL_LEFT;
            case 307: return KeyEvent.KEYCODE_ALT_RIGHT;
            case 308: return KeyEvent.KEYCODE_ALT_LEFT;
            default: return -1;
        }
    }

    private static View surface(View root) {
        if (root == null) return null;
        View best = null;
        if ((root instanceof SurfaceView || root instanceof TextureView) && root.isShown() &&
            root.getWidth() > 100 && root.getHeight() > 100) best = root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)root;
            for (int i = 0; i < group.getChildCount(); i++) {
                View found = surface(group.getChildAt(i));
                if (found != null && (best == null || found.getWidth() * found.getHeight() > best.getWidth() * best.getHeight()))
                    best = found;
            }
        }
        return best;
    }

    private static MotionEvent mouseEvent(int action, float x, float y, int buttonState) {
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(downTime == 0 ? now : downTime, now, action,
            x, y, 1, 1, 0, 1, 1, 0, 0);
        event.setSource(InputDevice.SOURCE_MOUSE);
        try {
            Method set = MotionEvent.class.getMethod("setButtonState", int.class);
            set.invoke(event, buttonState);
        } catch (ReflectiveOperationException ignored) {}
        return event;
    }

    private static void dispatchTouch(Activity activity, View view, MotionEvent event) {
        int[] surface = new int[2], window = new int[2];
        view.getLocationOnScreen(surface);
        activity.getWindow().getDecorView().getLocationOnScreen(window);
        event.offsetLocation(surface[0] - window[0], surface[1] - window[1]);
        activity.dispatchTouchEvent(event);
    }

    private static void mouse(Activity activity, View view, int action, int button) {
        if (mouseX < 0 || mouseY < 0) { mouseX = view.getWidth() / 2f; mouseY = view.getHeight() / 2f; }
        long now = SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) { buttons |= button; if (downTime == 0) downTime = now; }
        MotionEvent event = mouseEvent(action, mouseX, mouseY, buttons);
        dispatchTouch(activity, view, event);
        event.recycle();
        if (action == MotionEvent.ACTION_UP) {
            buttons &= ~button;
            if (buttons == 0) downTime = 0;
        }
    }

    private static void touch(Activity activity, View view, int kind, int id, float x, float y) {
        boolean present = touches.containsKey(id);
        if (kind == 8 && present || kind != 8 && !present) return;
        if (kind == 8) touches.put(id, new float[]{x,y});
        else if (kind == 9) touches.put(id, new float[]{x,y});
        int count = touches.size(), at = 0, chosen = 0;
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[count];
        for (Map.Entry<Integer,float[]> entry : touches.entrySet()) {
            MotionEvent.PointerProperties prop = new MotionEvent.PointerProperties();
            prop.id = entry.getKey(); prop.toolType = MotionEvent.TOOL_TYPE_FINGER;
            MotionEvent.PointerCoords coord = new MotionEvent.PointerCoords();
            coord.x = entry.getValue()[0]; coord.y = entry.getValue()[1];
            coord.pressure = 1; coord.size = 1;
            if (entry.getKey() == id) chosen = at;
            props[at] = prop; coords[at] = coord; at++;
        }
        int action = kind == 8 ? (count == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | chosen << 8) :
            kind == 10 ? (count == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | chosen << 8) : MotionEvent.ACTION_MOVE;
        long now = SystemClock.uptimeMillis();
        if (kind == 8 && count == 1) downTime = now;
        MotionEvent event = MotionEvent.obtain(downTime == 0 ? now : downTime, now, action, count,
            props, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        dispatchTouch(activity, view, event);
        event.recycle();
        if (kind == 10) {
            touches.remove(id);
            if (touches.isEmpty()) downTime = 0;
        }
    }

    private static final class Event implements Runnable {
        private final int kind, code;
        private final float x, y, vw, vh;
        Event(int kind, int code, float x, float y, float vw, float vh) {
            this.kind=kind; this.code=code; this.x=x; this.y=y; this.vw=vw; this.vh=vh;
        }
        @Override public void run() {
            Activity activity = owner.get();
            if (activity == null || !foreground()) return;
            View view = surface(activity.getWindow().getDecorView());
            if (view == null) return;
            long now = SystemClock.uptimeMillis();
            if (kind <= 1) {
                activity.dispatchKeyEvent(new KeyEvent(now, now, kind == 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                    key(code), 0, 0, -1, 0, 0, InputDevice.SOURCE_KEYBOARD));
                return;
            }
            float px = x * view.getWidth() / vw, py = y * view.getHeight() / vh;
            if (kind == 2 || kind == 3) {
                if (kind == 3) {
                    if (mouseX < 0 || mouseY < 0) { mouseX=view.getWidth()/2f; mouseY=view.getHeight()/2f; }
                    px += mouseX; py += mouseY;
                }
                mouseX=Math.max(0,Math.min(view.getWidth()-1,px));
                mouseY=Math.max(0,Math.min(view.getHeight()-1,py));
                MotionEvent move=mouseEvent(MotionEvent.ACTION_HOVER_MOVE,mouseX,mouseY,buttons);
                view.dispatchGenericMotionEvent(move); move.recycle();
            } else if (kind == 4 || kind == 5) {
                mouse(activity,view,kind == 4 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP,code);
            } else if (kind == 6) {
                if (mouseX < 0 || mouseY < 0) { mouseX=view.getWidth()/2f; mouseY=view.getHeight()/2f; }
                MotionEvent.PointerProperties property=new MotionEvent.PointerProperties();
                property.id=0; property.toolType=MotionEvent.TOOL_TYPE_MOUSE;
                MotionEvent.PointerCoords coord=new MotionEvent.PointerCoords();
                coord.x=mouseX; coord.y=mouseY; coord.setAxisValue(MotionEvent.AXIS_VSCROLL,x);
                MotionEvent scroll=MotionEvent.obtain(now,now,MotionEvent.ACTION_SCROLL,1,
                    new MotionEvent.PointerProperties[]{property},new MotionEvent.PointerCoords[]{coord},
                    0,buttons,1,1,0,0,InputDevice.SOURCE_MOUSE,0);
                view.dispatchGenericMotionEvent(scroll); scroll.recycle();
            } else if (kind == 7) {
                mouse(activity,view,MotionEvent.ACTION_DOWN,code);
                view.postDelayed(new Event(5,code,x,y,vw,vh),20);
            } else if (kind == 11) {
                touch(activity,view,8,code,px,py);
                view.postDelayed(new Event(10,code,x,y,vw,vh),20);
            } else touch(activity,view,kind == 8 ? 8 : kind == 9 ? 9 : 10,code,px,py);
        }
    }
}
