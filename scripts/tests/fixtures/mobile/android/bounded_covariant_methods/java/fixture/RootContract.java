package fixture;
public interface RootContract<C> {
    <M extends Bound> ResultBase<M> make(Pair<C, M> input);
}
