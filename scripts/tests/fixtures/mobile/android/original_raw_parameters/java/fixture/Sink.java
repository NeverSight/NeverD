package fixture;
public class Sink {
    public RawBox<?> seen;
    public Object tag;
    public int calls;
    public RuntimeException failure;
    public Sink() {}
    @SuppressWarnings("rawtypes")
    public void accept(RawBox box, Object tag) {
        Trace.events = Trace.events * 10 + 1;
        seen = box; this.tag = tag; calls++;
        if (failure != null) throw failure;
    }
}
