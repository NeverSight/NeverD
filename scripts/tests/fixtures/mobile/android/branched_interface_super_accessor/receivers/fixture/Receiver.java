package fixture;
public class Receiver implements Child {
    @Override public int read(long value) {
        Trace.receiver = this;
        Trace.input = value;
        Trace.hit(2);
        Trace.readAction.run();
        Trace.hit(3);
        return Trace.answer;
    }
    @Override public int measure(long value) {
        Trace.overrides++;
        Trace.hit(9);
        return Trace.other;
    }
    @Override public int side() { return 42; }
}
