package fixture;
public class Defaults {
    public static final Sink<Object> CB;
    static { Trace.hit(2); CB = new Consumer(); }
}
