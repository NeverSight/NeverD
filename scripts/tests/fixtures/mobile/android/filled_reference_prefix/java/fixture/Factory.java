package fixture;
public class Factory {
    static { Trace.hit(1); Trace.initialize.run(); Trace.hit(2); }
    public static Object[] prepare(Object[] values) {
        Trace.hit(3);
        Trace.supplied=values;
        Trace.prepare.run();
        Trace.hit(4);
        return Trace.substitute ? Trace.replacement : values;
    }
}
