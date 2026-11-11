package fixture;
public interface DefaultApi extends Marker {
    default void observe(Object[] values, long wide, double real) {
        DefaultTrace.calls++;
        DefaultTrace.hit(1);
        DefaultTrace.values = values;
        DefaultTrace.wide = wide;
        DefaultTrace.real = real;
        Object value = values[0];
        DefaultTrace.seen = value;
        if (DefaultTrace.failure != null) throw DefaultTrace.failure;
        DefaultTrace.hit(2);
    }
}
