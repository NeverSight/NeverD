package fixture;
public class Leaf<T> implements Base<T> {
    public T value;
    public Leaf(T value) { this.value = value; }
}
