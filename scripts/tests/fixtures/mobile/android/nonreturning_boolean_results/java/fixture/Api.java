package fixture;
public interface Api<E, S> {
    boolean add(E entry);
    boolean addAll(java.util.Collection<? extends E> entries);
    boolean push(S input);
}
