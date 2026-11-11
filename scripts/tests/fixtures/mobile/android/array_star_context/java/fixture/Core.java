package fixture;
public final class Core {
    private Core() {}
    public static <Value> Value[] call(Value[] values, int count, Class<?> token) {
        return Provider.pass(values, count, token);
    }
    public static <Value> Value[] wide(Value[] values, Class<?> token, long limit, double scale, boolean enabled) {
        return Provider.spread(values, token, limit, scale, enabled);
    }
    public static Object[] checked(Object values, int count, Object token) {
        return call((Object[]) values, count, (Class<?>) token);
    }
}
