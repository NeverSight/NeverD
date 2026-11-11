package fixture;
public final class Core {
    public static void send(Child child, Child.Left value) {
        Trace.hit(1);
        child.apply(value);
        Trace.hit(8);
    }
}
