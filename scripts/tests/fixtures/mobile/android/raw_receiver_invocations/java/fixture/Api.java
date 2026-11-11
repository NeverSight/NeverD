package fixture;
public interface Api<T> {
    <U> Box<? extends U> read(Box<U> input, T tag, long bits, double value);
}
