package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Carrier<P, Q> {
    private final Payload<Object, Object> backing;
    static {
        Trace.events = Trace.events * 10 + 2;
        Trace.initializations++;
    }
    private Carrier(Payload value) { backing = value; }
    public static final Carrier box(Payload value) { return new Carrier(value); }
    public Payload<Object, Object> payload() { return backing; }
}
