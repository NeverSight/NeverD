package fixture;
public final class Provider {
    public Fn<Object,Unit> first,second;
    public Fn<Object,Unit> read() { Trace.hit(2);return first; }
    public Fn<Object,Unit> write() { Trace.hit(4);return second; }
}
