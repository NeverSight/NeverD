package fixture;
public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Context context, String key, Token<?> token, Object result) {}
    public static void hit(int phase, Context context, String key, Token<?> token, Object result) {
        hook.observe(phase, context, key, token, result);
    }
}
