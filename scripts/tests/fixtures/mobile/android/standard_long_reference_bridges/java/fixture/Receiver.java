package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S, V> implements Function {
    public S result;
    public Receiver() {}
    public S apply(long input) {
        Trace.events = Trace.events * 10 + 1;
        Trace.first = input;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return result;
    }
    public Object invoke(Object input) {
        return apply(((Number) input).longValue());
    }
}
