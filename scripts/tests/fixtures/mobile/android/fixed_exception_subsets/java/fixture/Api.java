package fixture;
public interface Api {
    Object read(Object input) throws First, Second, UnusedFailure;
    <T> T unwrap(Class<T> kind) throws First, UnusedFailure;
}
