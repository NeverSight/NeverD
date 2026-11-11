package fixture;
public class Carrier<T> {
    public java.util.ArrayList<Object> values = new java.util.ArrayList<Object>();
    public Carrier() {}
    public boolean contains(Object seed) { Trace.hit(3, this, seed); return !Trace.forceContainsFalse && values.contains(seed); }
    public void add(T seed) { Trace.hit(4, this, seed); values.add(seed); }
    public void remove(T seed) { Trace.hit(5, this, seed); values.remove(seed); }
    public boolean empty() { Trace.hit(6, this, null); return Trace.emptyOverride < 0 ? values.isEmpty() : Trace.emptyOverride != 0; }
}
