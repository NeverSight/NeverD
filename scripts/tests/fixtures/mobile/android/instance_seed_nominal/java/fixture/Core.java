package fixture;
public final class Core {
    private Core() {}
    public static Provider receiver(){Trace.receivers++;Trace.hit(1);Trace.before.run();return Trace.source;}
    public static <T> Provider call(T value,boolean left,boolean right){return receiver().accept(value,left,right);}
    public static Provider checked(Object value,boolean left,boolean right){return call((String)value,left,right);}
}
