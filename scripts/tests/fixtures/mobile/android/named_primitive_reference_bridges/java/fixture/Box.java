package fixture;
public final class Box<T, V> {
    public T value;
    public V vector;
    public Box(T value, V vector) { this.value = value; this.vector = vector; }
}
