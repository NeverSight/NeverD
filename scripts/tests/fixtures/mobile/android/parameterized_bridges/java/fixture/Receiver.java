package fixture;

public class Receiver implements Function<Box<? super Value>, Box<? extends Signal>, Object> {
    public int calls;
    public Object first;
    public Object second;
    public Value value;
    public RuntimeException failure;
    public Object invoke(Box<? super Value> first, Box<? extends Signal> second) {
        ++calls;
        this.first = first;
        this.second = second;
        if (failure != null) throw failure;
        first.set(value);
        Signal result = second.get();
        return result;
    }
}
