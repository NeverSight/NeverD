package fixture;

public class Holder<T extends Api<? super T>> {
    public T stored;

    public Holder(T stored) { this.stored = stored; }
    public T value() { return stored; }
    public int read(T input) { return stored.read(input); }
}
