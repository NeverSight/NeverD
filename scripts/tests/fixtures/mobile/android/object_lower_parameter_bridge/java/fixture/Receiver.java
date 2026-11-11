package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Receiver<S> implements Function {
    public Object payload, result;
    public Consumer<Object> seen;
    public int calls;
    public Receiver() {}
    public Object invoke(Consumer<Object> consumer) {
        Trace.events = Trace.events * 10 + 1;
        calls++; seen = consumer;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        consumer.resume(payload);
        Trace.events = Trace.events * 10 + 3;
        return result;
    }
    public Object invoke(Object input) { return invoke((Consumer<Object>) input); }
}
