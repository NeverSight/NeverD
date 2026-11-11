package fixture;
public class Receiver<A,B> extends Root<A> implements RefinedIndex<A> {
    public Leaf<A> value;
    public int lower, upper, calls;
    public Receiver() {}
    public Leaf<A> create(int lower, int upper) {
        Trace.events = Trace.events * 10 + 1;
        calls++;
        this.lower = lower;
        this.upper = upper;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
    public static Base<?> checked(Object value) { return (Base<?>)value; }
}
