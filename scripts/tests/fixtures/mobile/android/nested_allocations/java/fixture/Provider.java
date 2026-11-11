package fixture;
public class Provider {
    public Object stored;
    public Object read() {
        Trace.events = Trace.events * 10 + 3; Trace.reads++;
        if (Trace.failAt == 3) throw Trace.failure;
        return stored;
    }
    public static byte[] bytes(int length) {
        Trace.events = Trace.events * 10 + 5; Trace.arrays++;
        if (Trace.failAt == 5) throw Trace.failure;
        return new byte[length];
    }
}
