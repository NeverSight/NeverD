package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S, V> implements Function {
    public S result;
    public Receiver() {}
    public S apply(double input) {
        Trace.events = Trace.events * 10 + 1;
        Trace.first = Double.doubleToRawLongBits(input);
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return result;
    }
    public Object invoke(Object input) {
        return apply(((Number) input).doubleValue());
    }
}
