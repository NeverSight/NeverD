package fixture;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Context context, Object result) {}
    public Object provide(Context context) { return context == null ? null : context.offered; }
    public RuntimeException onMissing(Context context) {
        return new IllegalStateException("owned missing holder");
    }
    public static void hit(int phase, Context context, Object result) {
        if (hook != null) hook.observe(phase, context, result);
    }
    public static Object fetch(Context context) {
        return hook == null ? (context == null ? null : context.offered) : hook.provide(context);
    }
    public static RuntimeException missing(Context context) {
        return hook == null ? new IllegalStateException("owned missing holder") : hook.onMissing(context);
    }
}
