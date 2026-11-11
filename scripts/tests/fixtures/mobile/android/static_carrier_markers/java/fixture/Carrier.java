package fixture;

import java.util.List;

public final class Carrier<T> implements Marker {
    public static final Object companion = new Object();
    public static int calls;
    private final Object value;

    private Carrier(Object value) { this.value = value; }
    public static final Carrier wrap(Object value) { return new Carrier(value); }
    public final Object unwrap() { return value; }
    private static Object use(Object carrier, List<Object> values) {
        ++calls;
        return carrier;
    }
}
