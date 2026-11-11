package fixture;
public final class ArrayStarContextCaller {
    public static <Cell> Cell[] call(Cell[] values, int count, Class<?> token) { return Core.call(values, count, token); }
    public static <Cell> Cell[] wide(Cell[] values, Class<?> token, long limit, double scale, boolean enabled) { return Core.wide(values, token, limit, scale, enabled); }
    public static Object[] checked(Object values, int count, Object token) { return Core.checked(values, count, token); }
}
