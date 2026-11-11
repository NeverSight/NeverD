package fixture;
public class Host {
    public int ticks;
    public final class Worker implements Api<Value> {
    public int calls;
    public Object seen;
    public RuntimeException failure;
    public <A extends Value, B extends A> A[] copy(A[] input, B tag, Sink<? super A> sink) {
        ticks++; calls++; seen = tag;
        if (failure != null) throw failure;
        sink.accept(tag, input);
        return input;
    }
    public <A extends Value> void resume(A tag, Sink<? super A> sink) {
        ticks++; calls++; seen = tag;
        if (failure != null) throw failure;
        sink.accept(tag, null);
    }
    public Value value(Value input) { return input; }
    }
}
