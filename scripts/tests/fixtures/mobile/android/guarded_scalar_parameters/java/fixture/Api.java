package fixture;
public interface Api<T> { boolean contains(Object input); boolean matches(Object input); <R, S> R echo(R input, S tag); }
