package fixture;

import java.util.List;

public final class Carrier<T> {
    private final Object value;
    private Carrier(Object value) { this.value = value; }
    public static final Carrier wrap(Object value) { return new Carrier(value); }
    public final Object unwrap() { return value; }
    public static Object use(Object carrier, List<Object> values) { return carrier; }
}
