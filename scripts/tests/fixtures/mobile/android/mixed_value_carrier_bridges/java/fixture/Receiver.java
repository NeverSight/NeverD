package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public Object first, second;
    public Object invoke(Object first, Object second, Object third) {
        CarrierA a = (CarrierA) first;
        CarrierB b = (CarrierB) second;
        long c = ((CarrierB) third).unwrap();
        return apply(a, b, c);
    }
    public final CarrierA apply(CarrierA first, CarrierB second, long third) {
        Trace.events = Trace.events * 10 + 3;
        this.first = first;
        this.second = second;
        Trace.second = third;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return first;
    }
}
