package fixture;
public final class Provider {
    public static Object current, late, style;
    public static Object style() {
        Trace.styleSeen = Trace.seen;
        Trace.step(4);
        return style;
    }
}
