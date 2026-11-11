package fixture;
public class Parent<K,V,T> {
    static { Trace.hit(8); }
    public final Node<K,V> node;
    public final Path<K,V,T>[] path;
    public Parent(Node<K,V> node, Path<K,V,T>[] path) {
        this.node = node; this.path = path;
        Trace.parent = this;
        Trace.hit(3);
        if (node != null) { Trace.element = path[0]; Trace.hit(4); }
        Trace.hit(5);
    }
    public Parent(Object node, Object[] path) {
        this.node = null; this.path = null;
        throw new AssertionError("wrong constructor overload");
    }
}
