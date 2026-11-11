package fixture;

public final class Owner {
    public static <S, V> Object make(S result) { return new Worker<S, V>(result); }
    public static <S, V> Object another(S result) { return new Worker<S, V>(result); }

    @SuppressWarnings("rawtypes")
    public static final class Worker<S, V> implements Function {
        public S result;
        public Worker(S result) { this.result = result; }
        public S apply(long input) {
            Trace.events = Trace.events * 10 + 1;
            Trace.first = input;
            if (Trace.targetFailure != null) throw Trace.targetFailure;
            return result;
        }
        public Object invoke(Object input) { return apply(((Number) input).longValue()); }
    }
}
