package fixture;
public interface Contract<P, Q> {
    P value(P next);
    Cell<P, Q> lookup(P input);
}
