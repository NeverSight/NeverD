package fixture;
public interface RefinedIndex<P> extends Indexed<P> {
    Leaf<P> create(int lower, int upper);
}
