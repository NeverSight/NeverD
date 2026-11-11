package fixture;
public interface OtherApi<T extends Comparable<? super T>> {
    boolean contains(T value);
    T get();
    boolean isEmpty();
}
