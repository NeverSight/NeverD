package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Context context, String key, Object array, Object item, int index) {}
    public static void hit(int phase, Context context, String key, Object array, Object item, int index) {
        hook.observe(phase, context, key, array, item, index);
    }
}
