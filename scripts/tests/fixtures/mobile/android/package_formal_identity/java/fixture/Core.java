package fixture;
public final class Core<Owner> implements Marker<Owner> {
    private Core() {}
    static <T> T check(T value) {
        Trace.hit(1, value);
        if (value == null) {
            Trace.hit(2, value);
            throw new NullPointerException();
        }
        Trace.hit(3, value);
        T result = value;
        Trace.hit(4, result);
        return result;
    }
    public static Object call(Object value) { return check(value); }
    public static Base typed(Object value) { return (Base)check(value); }
}
