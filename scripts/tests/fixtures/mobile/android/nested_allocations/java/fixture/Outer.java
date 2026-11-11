package fixture;
@SuppressWarnings("unchecked")
public final class Outer<T> {
    static {
        Trace.events = Trace.events * 10 + 1;
        if (Trace.failAt == 1) throw Trace.failure;
    }
    public Inner inner;
    public byte[] bytes;
    public T payload;
    public Outer(Inner item, byte[] data) {
        Trace.events = Trace.events * 10 + 6; Trace.outers++;
        if (Trace.failAt == 6) throw Trace.failure;
        inner = item; bytes = data; payload = (T)item.value;
    }
}
