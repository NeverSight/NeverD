package fixture;
public class Provider {
    public static Object pick(Object fallback, Class<?>[] kinds, Object[] values) {
        Trace.hit(3); Trace.providerKinds = kinds; Trace.providerValues = values;
        Object value = values[0]; Trace.hit(4);
        return value == null ? fallback : value;
    }
}
