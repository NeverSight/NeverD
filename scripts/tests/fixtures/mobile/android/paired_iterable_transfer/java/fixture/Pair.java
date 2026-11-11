package fixture;
public class Pair<K, V> {
    public K first;
    public V second;
    public Pair(K first, V second) { this.first = first; this.second = second; }
    public K component1() { return first; }
    public V component2() { return second; }
}
