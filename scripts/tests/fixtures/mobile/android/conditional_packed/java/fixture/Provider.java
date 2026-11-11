package fixture;
public final class Provider {
    static { Trace.event(2); }
    public static Object read(Object value) { Trace.seen=value; Trace.reads++; Trace.event(3); return Trace.result; }
    public static int after() { int value=Trace.tail; Trace.event(4); Trace.tail=99; return value; }
}
