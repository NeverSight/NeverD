package fixture;

public final class Client {
    private Client() {}
    public static Object exchange(View<Object,Object> entry, Object input) {
        Object result = entry.setValue(input);
        Trace.after = result;
        Trace.hit(7);
        return result;
    }
    public static void write(View<Object,Object> entry, Object input) {
        entry.restore(input);
        Trace.hit(9);
    }
}
