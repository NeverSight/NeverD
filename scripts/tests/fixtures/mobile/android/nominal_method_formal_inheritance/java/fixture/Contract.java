package fixture;
public interface Contract<C> {
    <M extends Bound> Pair<M, M> make(Pair<C, M> input);
}
