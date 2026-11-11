package fixture;
public interface Contract<C> {
    <M extends Bound<M>> Pair<M, M> make(Pair<C, M> input);
}
