package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    private Core() {}
    public static Leaf receiver(){Trace.receivers++;Trace.hit(7);Trace.before.run();return Trace.source;}
    public static Enum typed(Class token,String key){return receiver().extra(token,key);}
    public static Enum erased(Class token,Object key){return ((Reader)receiver()).extra(token,key);}
}
