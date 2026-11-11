package fixture;
public final class Receiver implements Service {
    public Value result, last;
    public Consumer<? super Value> seen;
    public int calls;
    public Receiver() {}
    public Object request(Value input, Consumer<? super Value> consumer) {
        Trace.events = Trace.events * 10 + 1;
        calls++; last = input; seen = consumer;
        if (Trace.targetFailure != null) throw Trace.targetFailure;
        consumer.resume(input);
        Trace.events = Trace.events * 10 + 3;
        return result;
    }
}
