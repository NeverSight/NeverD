package fixture;
public interface OtherApi<T extends Comparable<? super T>> {
    boolean contains(T value);
    boolean before(T left, T right);
    T get();
    boolean isEmpty();
}
