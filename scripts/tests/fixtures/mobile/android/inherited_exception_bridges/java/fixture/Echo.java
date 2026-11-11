package fixture;

public interface Echo<T> {
    T echo(T item) throws java.io.IOException, ReflectiveOperationException;
}
