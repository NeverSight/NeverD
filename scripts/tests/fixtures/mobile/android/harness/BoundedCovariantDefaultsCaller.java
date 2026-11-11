import fixture.*;
public class BoundedCovariantDefaultsCaller {
    public static <R extends Bound> ResultLeaf<R> narrow(Provider owner, Pair<String, R> input) {
        return owner.make(input);
    }
    public static <M extends Bound> ResultBase<M> wide(Root<String> owner, Pair<String, M> input) {
        return owner.make(input);
    }
}
