package fixture;
public final class Inner {
    static {
        Trace.hit(4);
        if (Trace.holder != null) Trace.holder.array = Trace.replaced;
    }
    public Inner() {
        Trace.innerConstructors++;
        Trace.inner = this;
        Trace.hit(5);
    }
}
