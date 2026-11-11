package fixture;
public interface Contract<C> extends RootContract<C> {
    <M extends Bound> ResultMid<M> make(Pair<C, M> input);
}
