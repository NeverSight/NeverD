package fixture;

public class ArrayReceiver implements Function<Box<Value>[], Box<Signal>[], Object> {
    public int calls;
    public Object first;
    public Object second;
    public RuntimeException failure;
    public Object invoke(Box<Value>[] first, Box<Signal>[] second) {
        ++calls;
        this.first = first;
        this.second = second;
        if (failure != null) throw failure;
        Signal result = second[0].get();
        return result;
    }
}
