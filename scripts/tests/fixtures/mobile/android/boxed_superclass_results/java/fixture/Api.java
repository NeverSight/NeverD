package fixture;
import java.util.function.Consumer;
public interface Api<T> {
    T get(int index);
    default T at(int index) { return get(index); }
    default void each(int index, Consumer<? super T> action) {
        if (action == null) throw new NullPointerException("owned-action");
        action.accept(get(index));
        action.accept(get(index + 1));
    }
}
