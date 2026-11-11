package fixture;
public class Pair<K,V> {
    public final Object first,second;
    public Pair(Object first,Object second) { this.first=first;this.second=second; }
    @SuppressWarnings("unchecked") public K first() { Trace.hit(4);if(first!=null)((int[])first)[0]++;return (K)first; }
    @SuppressWarnings("unchecked") public V second() { Trace.hit(5);if(second!=null)((int[])second)[0]+=3;return (V)second; }
}
