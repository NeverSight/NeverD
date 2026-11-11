package fixture;

public class Trace {
    public static Trace hook;
    public Trace() {}
    public void observe(int phase, Core receiver, Provider provider, int index) {}
    public static void hit(int phase, Core receiver, Provider provider, int index) {
        Trace current = hook;
        if (current != null) current.observe(phase, receiver, provider, index);
    }
    public static int step(int index, Core receiver, Provider provider) {
        Trace.hit(4, receiver, provider, index);
        return receiver.state * 31 + index;
    }
}
