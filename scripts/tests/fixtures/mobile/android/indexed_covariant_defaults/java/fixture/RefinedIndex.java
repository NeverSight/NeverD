package fixture;
public interface RefinedIndex<P> extends Indexed<P> {
    Leaf<P> select(int lower, int upper);
    default Leaf<P> create(int lower, int upper) {
        Trace.events = Trace.events * 10 + 6;
        return select(lower, upper);
    }
}
