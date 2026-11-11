package fixture;
import java.util.function.Consumer;

public interface Api<T> {
    T next();
    boolean hasNext();
    void remove();
    <R> R echo(R input);
    default void forEachRemaining(Consumer<? super T> action) {
        if (action == null) throw new NullPointerException("owned-action");
        while (hasNext()) action.accept(next());
    }
}
