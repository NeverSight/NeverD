package fixture;
public final class Fallback {
    static { Trace.step(4); }
    public static Object value = Trace.token;
}
