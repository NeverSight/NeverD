package fixture;
public interface Api<T> {
    default T echo(T value) { return Trace.echo(value); }
}
