package fixture;
public final class Core {
    private Core() {}
    public static <Value> int call(Value[] values, Object context, int offset) {
        return Provider.measure(values, context, offset);
    }
    public static <Value> int wide(Value[] values, Object context, long limit, double scale, boolean enabled) {
        return Provider.measureWide(values, context, limit, scale, enabled);
    }
    public static int checked(Object values, Object context, int offset) {
        return call((Object[]) values, context, offset);
    }
}
