package fixture;

public class Provider {
    public int first() { Trace.hit(1); return Trace.seed + 1; }
    public int choose(int event) { Trace.hit(event); return Trace.seed + event; }
    public void step() {
        Trace.hit(5);
        if (Trace.stepFailure != null) throw Trace.stepFailure;
    }
    public void recover() { Trace.hit(6); }
    public void cleanup() { Trace.hit(7); }
    public int last() { Trace.hit(8); return Trace.seed + 8; }
}
