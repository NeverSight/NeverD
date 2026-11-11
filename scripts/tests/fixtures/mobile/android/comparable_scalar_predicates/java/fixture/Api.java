package fixture;
public interface Api<T extends Comparable<? super T>> {
    boolean contains(T value);
    T get();
    boolean isEmpty();
}
