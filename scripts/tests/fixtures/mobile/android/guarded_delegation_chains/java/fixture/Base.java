package fixture;
public class Base {
    public final Object token;
    public final int count;
    public final long wide;
    public final double number;
    public Base(Object token, int count, long wide, double number) {
        Trace.event(4);
        this.token = token; this.count = count; this.wide = wide; this.number = number;
        Trace.observed = this;
    }
}
