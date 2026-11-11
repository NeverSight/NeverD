package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Provider {
    public int calls;
    static { Trace.hit(1); }
    private static final class Carrier<T> {
        public final Producer<T> producer;
        static { Trace.hit(9); }
        public Carrier(Producer<T> producer) { Trace.hit(2); this.producer = producer; }
    }
    private final <T> void dispatch(int flags, Object input, Carrier<T> carrier, long bits) {
        Trace.hit(4); Trace.receiver = this; calls++;
        if (carrier != null && carrier.producer != null) {
            T result = carrier.producer.get(flags, input, bits);
            Trace.seen = result;
            Trace.hit(6);
            if (result != null) ((int[]) (Object) result)[0]++;
        }
        Trace.hit(7);
    }
    public void call(boolean present, int flags, Object input, Producer producer, long bits) {
        Trace.hit(3);
        Carrier carrier = present ? new Carrier(producer) : null;
        dispatch(flags, input, carrier, bits);
        Trace.hit(8);
    }
}
