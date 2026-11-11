package fixture;
public final class Carrier<T> {
    public final T first, second;
    public Carrier(T first, T second) {
        Trace.captured = this;
        Trace.hit(4);
        this.first = first;
        Trace.hit(5);
        this.second = second;
        Trace.hit(6);
    }
    public T first() { return first; }
    public T second() { return second; }
}
