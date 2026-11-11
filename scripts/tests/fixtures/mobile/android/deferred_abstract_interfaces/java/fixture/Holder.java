package fixture;

public abstract class Holder<S, V> implements Contract<S>, Mirror<S> {
    protected S state;

    public Holder() {
    }

    public final S seed() {
        return state;
    }
}
