package fixture;
public interface Api<I> {
    I read(I input);
    <T> T unwrap(Class<T> kind);
}
