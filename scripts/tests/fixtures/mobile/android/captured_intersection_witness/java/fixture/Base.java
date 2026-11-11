package fixture;
public class Base<E> implements First, Second {
    public Base(Object context, int index, String[] values) {
        Trace.hit(2);
        Trace.captured = owner(); Trace.contextSeen = context;
        Trace.indexSeen = index; Trace.inputs = values;
        Trace.construct.run(); Trace.hit(3);
    }
    public Object owner() { return null; }
    public void touch() { Trace.hit(6); Trace.action.run(); }
}
