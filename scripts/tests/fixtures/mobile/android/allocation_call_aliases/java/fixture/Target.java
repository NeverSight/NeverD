package fixture;
public final class Target {
    static {
        Trace.hit(1);
        if (Trace.holder != null) {
            Trace.holder.first = Trace.first;
            Trace.holder.second = Trace.second;
        }
    }
    public Object first, second;
    public boolean flag;
    public Target(Object first, Object second, boolean flag) {
        Trace.constructors++;
        Trace.hit(4);
        this.first = first;
        this.second = second;
        this.flag = flag;
        if (Trace.holder != null) Trace.holder.second = Trace.replaced;
    }
}
