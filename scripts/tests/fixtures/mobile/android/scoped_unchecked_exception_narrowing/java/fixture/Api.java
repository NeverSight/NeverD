package fixture;
public interface Api<I> {
    I read(I input) throws RequiredFailure;
    <T> T unwrap(Class<T> kind) throws RequiredFailure;
}
