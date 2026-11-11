package fixture;

public class Cell<K, V> {
    public K left;
    public V right;

    public Cell(K left, V right) {
        this.left = left;
        this.right = right;
    }
}
