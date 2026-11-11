package fixture;

public class Receiver<S, V> extends Holder<S, V> implements Contract<S>, Mirror<S> {
    public int lastIndex;
    public RuntimeException failure;

    public Receiver() {
        super();
    }

    public void insert(int index, S value) {
        lastIndex = index;
        if (failure != null) {
            throw failure;
        }
        state = value;
    }

    public S current() {
        return state;
    }

    public S tail() {
        return state;
    }
}
