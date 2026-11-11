package fixture;
public interface Provider extends Root<String> {
    default <R extends Bound> ResultLeaf<R> make(Pair<String, R> input) {
        Effects.enter(this, input);
        if (Effects.failure != null) throw Effects.failure;
        R value = input.second;
        return new ResultLeaf<R>(value);
    }
}
