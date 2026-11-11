package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Box<CarrierA> result;
    public Object invoke(Object first, Object second) {
        long a = ((CarrierA) first).unwrap();
        long b = ((CarrierB) second).unwrap();
        return apply(a, b);
    }
    public final Box<CarrierA> apply(long first, long second) {
        Trace.events = Trace.events * 10 + 3;
        Trace.first = first;
        Trace.second = second;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return result;
    }
}
