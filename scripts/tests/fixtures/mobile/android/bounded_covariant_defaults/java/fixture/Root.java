package fixture;
public interface Root<P> {
    <M extends Bound> ResultBase<M> make(Pair<P, M> input);
}
