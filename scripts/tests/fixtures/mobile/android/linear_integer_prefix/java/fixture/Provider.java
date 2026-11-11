package fixture;
public final class Provider {
    public boolean flag;
    public boolean read() { Trace.reads++; Trace.seen = this; Trace.hit(1); return flag; }
}
