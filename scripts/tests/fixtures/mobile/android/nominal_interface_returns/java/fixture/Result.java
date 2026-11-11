package fixture;
public interface Result<T> {
    T read();
    void write(T value);
}
