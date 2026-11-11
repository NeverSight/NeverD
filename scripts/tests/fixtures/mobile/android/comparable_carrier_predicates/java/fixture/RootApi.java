package fixture;
public interface RootApi<T extends Comparable<? super T>> {
    boolean contains(T value);
    T get();
    boolean isEmpty();
}
