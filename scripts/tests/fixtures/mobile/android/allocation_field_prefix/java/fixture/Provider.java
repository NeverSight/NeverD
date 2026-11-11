package fixture;
public final class Provider {
    public static Object read(Input input) {
        Trace.reads++;
        Trace.hit(2);
        if (Trace.replace) input.owner.input = input.next;
        return input.value;
    }
}
