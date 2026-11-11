package fixture;
public interface RefinedIndex<P> {
    Leaf<P> create(P lower, int upper);
}
