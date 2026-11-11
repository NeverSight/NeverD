package fixture;
public class Builder<K,V> {
    @SuppressWarnings("unchecked")
    public Node<K,V> fetch() {
        Trace.getterReceiver = this;
        Trace.hit(1);
        if (Trace.path != null && Trace.path.length != 0) Trace.path[0] = Trace.changed;
        Trace.path = Trace.replacementPath;
        Trace.builder = Trace.other;
        Node<K,V> selected = (Node<K,V>)Trace.returned;
        Trace.hit(2);
        return selected;
    }
    public int stamp() {
        Trace.stampReceiver = this;
        Trace.hit(6);
        return Trace.stampValue;
    }
}
