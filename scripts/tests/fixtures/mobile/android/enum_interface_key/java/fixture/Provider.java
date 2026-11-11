package fixture;
public class Provider implements View<Object> {
    @SuppressWarnings("unchecked")
    public <E extends Enum<E>> E select(Class<E> type,Object key,E fallback){Trace.calls++;Trace.receiver=this;Trace.type=type;Trace.key=key;Trace.fallback=fallback;Trace.hit(1);Trace.action.run();Trace.hit(2);return (E)Trace.result;}
}
