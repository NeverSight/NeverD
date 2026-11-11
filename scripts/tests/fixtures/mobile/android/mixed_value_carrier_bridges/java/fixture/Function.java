package fixture;
public interface Function<P,Q,R,S> {
    S invoke(P first, Q second, R third);
}
