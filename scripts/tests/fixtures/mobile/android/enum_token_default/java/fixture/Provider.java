package fixture;
public class Provider {
    public static <E extends Enum<E>> E convert(Class<E> type,Object value) {return convert(type,value,null);}
    @SuppressWarnings("unchecked")
    public static <E extends Enum<E>> E convert(Class<E> type,Object value,E fallback) {
        Trace.calls++;Trace.type=type;Trace.value=value;Trace.fallback=fallback;Trace.hit(1);
        Trace.action.run();Trace.hit(2);return (E)Trace.result;
    }
}
