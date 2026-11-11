package fixture;
public class ResultLeaf<V extends Bound> implements ResultMid<V> {
    public V value;
    public ResultLeaf(V value) { this.value = value; }
}
