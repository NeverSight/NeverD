package fixture;
public interface Api<T> {
    boolean contains(Object input);
    int index(Object input);
    <R, S> R echo(R input, S tag);
}
