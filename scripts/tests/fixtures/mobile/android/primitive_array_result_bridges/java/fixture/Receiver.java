package fixture;
@SuppressWarnings("rawtypes")
public final class Receiver<S> implements Function {
    public int[] result;
    public int calls;
    public Object invoke(Object first, Object second) {
        return apply(((Number) first).intValue(), ((Number) second).intValue());
    }
    public final int[] apply(int first, int second) {
        Trace.events = Trace.events * 10 + 3;
        Trace.first = first;
        Trace.second = second;
        calls++;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        return result;
    }
}
