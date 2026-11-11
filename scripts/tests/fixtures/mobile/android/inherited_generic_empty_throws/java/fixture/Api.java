package fixture;
public interface Api<T> {
    T convert(Object input, T fallback) throws java.io.IOException, ReflectiveOperationException;
}
