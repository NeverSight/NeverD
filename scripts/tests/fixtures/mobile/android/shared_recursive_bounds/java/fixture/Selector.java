package fixture;

public interface Selector<P, R> {
    R select(P value);
}
