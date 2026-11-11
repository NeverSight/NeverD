package fixture;
public class Holder<T extends Base> {
    public final T value;
    public Holder(T value) { this.value = value; }
    public T get() { return value; }
}
