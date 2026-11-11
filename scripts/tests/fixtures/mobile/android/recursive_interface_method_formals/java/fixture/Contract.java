package fixture;
public interface Contract<C> {
    <M extends Bound<? super M>> Pair<M, M> make(Pair<C, M> input);
}
