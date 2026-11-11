package fixture;
public final class Fields {
    public static int number;
    static { Trace.event(3); number = Trace.initialNumber; }
}
