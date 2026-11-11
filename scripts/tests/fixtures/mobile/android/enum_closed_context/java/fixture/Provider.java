package fixture;
public class Provider {
    @SuppressWarnings("unchecked")
    public static <E extends Enum<E>> E select(Class<E> type,String text) {Trace.calls++;Trace.type=type;Trace.text=text;Trace.hit(1);Trace.action.run();Trace.hit(2);return (E)Trace.result;}
}
