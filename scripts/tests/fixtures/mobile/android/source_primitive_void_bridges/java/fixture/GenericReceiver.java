package fixture;
public class GenericReceiver<P> implements Value<P> {
    public P value;
    public GenericReceiver() {}
    public P get() { return value; }
    public void set(P next) { Trace.mark(5); value = next; }
}
