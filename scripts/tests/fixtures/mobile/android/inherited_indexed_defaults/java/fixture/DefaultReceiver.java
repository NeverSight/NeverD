package fixture;
public class DefaultReceiver<L,R> implements InheritedIndex<L,R> {
    public Leaf<R> value;
    public int lower, upper, calls;
    public DefaultReceiver() {}
    public Leaf<R> select(int lower, int upper) {
        Trace.events = Trace.events * 10 + 7;
        calls++;
        this.lower = lower;
        this.upper = upper;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
}
