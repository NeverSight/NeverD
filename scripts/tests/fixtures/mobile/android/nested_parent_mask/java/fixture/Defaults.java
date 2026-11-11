package fixture;
public final class Defaults {
    public static final Sink<Object> CB;
    static { Trace.hit(1);CB=new Consumer();Trace.hit(2); }
    public static long defaultBits() { Trace.hit(3);return Trace.defaultBits; }
}
