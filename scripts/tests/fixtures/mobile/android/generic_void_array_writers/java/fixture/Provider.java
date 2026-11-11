package fixture;
public class Provider {
    static { Trace.hit(1); }
    public static <T> void fill(T[] values, T value, int begin, int end) {
        Trace.source = values; Trace.destination = value; Trace.hit(2);
        java.util.Arrays.fill(values, begin, end, value);
        Trace.hit(3);
    }
}
