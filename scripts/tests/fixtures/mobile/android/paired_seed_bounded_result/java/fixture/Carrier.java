package fixture;
public final class Carrier<T> {
    public final T stored;
    public Carrier(T stored) { this.stored=stored; }
    public T value() { return stored; }
}
