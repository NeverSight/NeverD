package fixture;
public final class ArrayObjectContextCaller {
    public static <Cell> int call(Cell[] values, Object context, int offset) {
        return Core.call(values, context, offset);
    }
    public static <Cell> int wide(Cell[] values, Object context, long limit, double scale, boolean enabled) {
        return Core.wide(values, context, limit, scale, enabled);
    }
    public static int checked(Object values, Object context, int offset) {
        return Core.checked(values, context, offset);
    }
}
