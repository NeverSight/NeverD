package fixture;
public final class Atom<A> {
    public A value;
    public Atom(A value) {
        this.value = value;
        Trace.allocations++; Trace.created = this;
    }
}
