package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Object invoke(Object value) {
        return apply(((Number) value).intValue());
    }
    public final Integer apply(int value) {
        Trace.events = Trace.events * 10 + 3;
        Trace.first = value;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return Integer.valueOf(value);
    }
}
