package fixture;
public class Parent {
    public RawBox<?> seen;
    public Object tag;
    public int calls;
    public RuntimeException failure;
    public Parent() {}
    @SuppressWarnings("rawtypes")
    public void accept(RawBox box, Object tag) {
        Trace.events = Trace.events * 10 + 2;
        seen = box; this.tag = tag; calls++;
        if (failure != null) throw failure;
    }
}
