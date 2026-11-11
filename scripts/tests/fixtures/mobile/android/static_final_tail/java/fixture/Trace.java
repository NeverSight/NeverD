package fixture;

public class Trace {
    public static Trace hook;

    public Trace() {}
    public void observe(int phase, Object token, float[] data) {}
    public void onCleanup(Object token) {}
    public int answer(Object token) { return 17; }

    public static void inside(int phase, Object token, float[] data) {
        if (hook != null) hook.observe(phase, token, data);
    }

    public static void cleanup(Object token) {
        if (hook != null) hook.onCleanup(token);
    }

    public static int finish(Object token) {
        return hook == null ? 17 : hook.answer(token);
    }
}
