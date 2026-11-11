package fixture;
public class Finder<C> {
    static { Trace.hit(9); }
    public static <T> boolean test(Carrier<T> values, Object input) {
        Trace.hit(1);
        boolean result = values.accepts(input);
        Trace.hit(4);
        return result;
    }
}
