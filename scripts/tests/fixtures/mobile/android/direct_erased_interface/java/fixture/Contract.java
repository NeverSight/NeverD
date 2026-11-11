package fixture;

public interface Contract<P, R> {
    R map(P input);
    Cursor<P> items();
}
