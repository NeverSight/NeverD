package fixture;

public class Parent<T> extends Root implements Counter, GenericSink<T> {
    public Parent(int seed) { super(seed); }
    public T echo(T value) { return value; }
    public int view(int position) { return seed + position; }
}
