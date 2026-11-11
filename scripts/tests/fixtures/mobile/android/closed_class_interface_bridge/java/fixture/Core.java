package fixture;
public final class Core {
    private Core() {}
    public static Leaf receiver(){Trace.receivers++;Trace.hit(7);Trace.before.run();return Trace.source;}
    public static Result typed(String key){return receiver().read(key);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Result erased(Object key){return ((Reader)receiver()).read(key);}
}
