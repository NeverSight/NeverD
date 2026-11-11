package fixture;
public class Core<A,B> extends Root<A> implements RefinedIndex<A> {
    public Leaf<A> value;
    public A lower;
    public int upper, calls;
    public Core() {}
    public Leaf<A> create(A lower, int upper) {
        Trace.events = Trace.events * 10 + 1;
        calls++;
        this.lower = lower;
        this.upper = upper;
        if (Trace.failure != null) throw Trace.failure;
        return value;
    }
    public static Base<?> checked(Object value) { return (Base<?>)value; }
}
