package fixture;
public interface Api<T extends Comparable<? super T>> {
    boolean contains(T value);
    boolean isEmpty();
    T lower();
    T observe(T value);
}
