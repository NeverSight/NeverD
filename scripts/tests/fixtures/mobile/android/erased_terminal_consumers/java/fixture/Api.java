package fixture;
import java.util.Collection;
public interface Api<T> extends Parent<T> {
    boolean add(T input); boolean bulk(Collection<? extends T> input); boolean containsAll(Collection<?> input);
    <R, S> R echo(R input, S tag); <R> R[] array(R[] input);
}
