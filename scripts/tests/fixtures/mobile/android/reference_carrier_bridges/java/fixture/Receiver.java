package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Payload seen;
    public int calls;
    public Object invoke(Object value) {
        return apply(((Carrier) value).unwrap());
    }
    public final Boolean apply(Payload value) {
        Trace.events = Trace.events * 10 + 3;
        Trace.first = value == null ? 0 : value.value;
        seen = value;
        calls++;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return Boolean.valueOf(value != null && (value.value & 1) != 0);
    }
}
