package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Receiver<S, V> implements Function {
    public Box<S> first;
    public CarrierA second;
    public Receiver() {}
    public Box<S> apply(Box<S> left, CarrierA right, int number) {
        Trace.events = Trace.events * 10 + 1;
        first = left; second = right; Trace.first = number;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return left;
    }
    public Object invoke(Object left, Object right, Object number) {
        return apply((Box<S>) left, (CarrierA) right, ((Number) number).intValue());
    }
}
