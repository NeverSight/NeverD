package fixture;
public final class Target {
    static {
        Trace.hit(1);
        if (Trace.holder != null) Trace.holder.array = Trace.prepared;
    }
    public int length;
    public Object marker;
    public boolean flag;
    public Target(int length, Object marker, boolean flag) {
        Trace.constructors++;
        Trace.target = this;
        Trace.hit(2);
        this.length = length;
        this.marker = marker;
        this.flag = flag;
        if (Trace.holder != null) Trace.holder.array = Trace.replaced;
    }
}
