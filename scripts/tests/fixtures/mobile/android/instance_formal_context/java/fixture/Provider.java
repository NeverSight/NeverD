package fixture;
import java.lang.reflect.Type;
public class Provider {
    @SuppressWarnings("unchecked")
    public <E> E convert(Type type,Object value,E fallback) throws Failure {
        Trace.calls++;Trace.receiver=this;Trace.type=type;Trace.value=value;Trace.fallback=fallback;Trace.hit(1);
        Trace.action.run();Trace.hit(2);return (E)Trace.result;
    }
}
