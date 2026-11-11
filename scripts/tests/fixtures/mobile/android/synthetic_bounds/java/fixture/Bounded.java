package fixture;

public interface Bounded<E extends Comparable<? super E>> {
    boolean accept(E value);
}
