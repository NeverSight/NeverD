package fixture;
import java.lang.reflect.Type;
public class Provider {
    public static <T> T convert(Class<T> type,Object value,T fallback)throws Failure{Trace.tokenCalls++;Trace.token=type;Trace.hit(1);return opaque(type,value,fallback);}
    @SuppressWarnings("unchecked")
    public static <T> T opaque(Type type,Object value,T fallback)throws Failure{Trace.calls++;Trace.type=type;Trace.value=value;Trace.fallback=fallback;Trace.hit(2);Trace.action.run();Trace.hit(3);return (T)Trace.result;}
}
