package fixture;
public final class Provider {
    public int branch,flag;
    public volatile Object first,alternate;
    public long finish(long bits) {
        Trace.calls++;Trace.step(2);Trace.input=bits;Trace.step(3);return Trace.returned;
    }
}
