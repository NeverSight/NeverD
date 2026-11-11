package fixture;
public interface Bucket<E> {
    boolean contains(E value);
    void push(E value);
    E pop();
}
