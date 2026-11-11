package fixture;
public interface Api<T extends Comparable<? super T>> extends RootApi<T> {
    boolean contains(T value);
    boolean before(T left, T right);
}
