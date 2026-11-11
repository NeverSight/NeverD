package fixture;
public interface Contract<P, Q> {
    P value(P next);
    default Cell<P, Q> lookup(P input) {
        Trace.defaults++;
        throw new AssertionError("unexpected interface default");
    }
}
