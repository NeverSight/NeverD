package fixture;
public final class Derived extends Provider {
    Object read(Object input) {
        Trace.seen = input;
        Trace.reads++;
        Trace.hit(3);
        if (input != null) ((int[]) input)[0]++;
        return Trace.alternate;
    }
}
