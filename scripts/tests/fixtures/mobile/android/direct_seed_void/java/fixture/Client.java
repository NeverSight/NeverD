package fixture;
public final class Client {
    public static void direct(State state, int first, Object a, int second, Object b) {
        Provider.write(state, first, a, second, b);
        Trace.hit(7);
    }
    public static void inherited(State state, int first, Object a, int second, Object b) {
        Facade.write(state, first, a, second, b);
        Trace.hit(7);
    }
    public static void wide(State state, int first, Object a, int second, Object b, long bits) {
        Provider.writeWide(state, first, a, second, b, bits);
        Trace.hit(7);
    }
    public static void inheritedWide(State state, int first, Object a, int second, Object b, long bits) {
        Facade.writeWide(state, first, a, second, b, bits);
        Trace.hit(7);
    }
}
