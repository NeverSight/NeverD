package fixture;
public final class Carrier<T> {
    public final T first, second;
    public Carrier(T first, T second) { this.first=first; this.second=second; }
    public T first() { return first; }
    public T second() { return second; }
}
