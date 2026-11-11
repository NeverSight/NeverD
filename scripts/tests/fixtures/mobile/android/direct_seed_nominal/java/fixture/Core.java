package fixture;
public final class Core {
    private Core() {}
    public static Provider receiver(){Trace.receivers++;Trace.hit(1);Trace.before.run();return Trace.source;}
    public static <T> Provider call(T value){return receiver().accept(value);}
    public static Provider checked(Object value){return call((String)value);}
}
