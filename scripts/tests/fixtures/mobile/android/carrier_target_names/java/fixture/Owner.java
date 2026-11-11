package fixture;
public final class Owner {
    @SuppressWarnings("rawtypes")
    public static Function make() {
        return new Function() {
            public Object invoke(Object first, Object second) {
                long a = ((CarrierA) first).unwrap();
                long b = ((CarrierB) second).unwrap();
                return CarrierB.wrap(apply(a, b));
            }
            public final long apply(long first, long second) {
                Trace.events = Trace.events * 10 + 3;
                Trace.first = first;
                Trace.second = second;
                if (Trace.targetFailure != null) throw Trace.targetFailure;
                return second;
            }
        };
    }
}
