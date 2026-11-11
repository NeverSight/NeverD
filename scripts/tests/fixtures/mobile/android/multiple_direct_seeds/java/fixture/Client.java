package fixture;
public final class Client {
    public static void direct3(State state, int first, Object a, int second, Object b, int third, Object c) {
        Provider.write3(state, first, a, second, b, third, c);
        Trace.hit(7);
    }
    public static void inherited3(State state, int first, Object a, int second, Object b, int third, Object c) {
        Facade.write3(state, first, a, second, b, third, c);
        Trace.hit(7);
    }
    public static void direct4(State state, int first, Object a, int second, Object b, int third, Object c, int fourth, Object d) {
        Provider.write4(state, first, a, second, b, third, c, fourth, d);
        Trace.hit(7);
    }
    public static void inherited4(State state, int first, Object a, int second, Object b, int third, Object c, int fourth, Object d) {
        Facade.write4(state, first, a, second, b, third, c, fourth, d);
        Trace.hit(7);
    }
}
