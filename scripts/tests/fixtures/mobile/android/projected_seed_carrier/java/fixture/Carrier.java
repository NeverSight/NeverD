package fixture;
public final class Carrier<T> {
    private final Object value;
    private Carrier(Object value) { this.value = value; }
    public static final Carrier wrap(Object value) { return new Carrier(value); }
    public final Object unwrap() { return value; }
    public static <V> void send(Object carrier, V value, Callback<? super Object,? super V,Object> callback) {
        Trace.hit(1);
        Object result = callback.apply(carrier, value);
        Trace.result = result;
        Trace.hit(3);
    }
}
