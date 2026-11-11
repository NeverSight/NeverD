package fixture;
public class Parent<K,V> {
    static { Trace.hit(1); }
    public Base<K,V> selected;
    public Parent(Base<K,V> value) { Trace.partial = this; Trace.hit(4); selected = value; }
    public Parent(Derived<K,V> value) { Trace.partial = this; Trace.hit(9); selected = value; }
}
