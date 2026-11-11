import fixture.Client;
import java.lang.reflect.Type;
public final class InstanceFormalContextCaller {
    public static Object defaults(Type type,Object value,Object fallback) {return Client.<Object>call(type,value,fallback);}
    public static Object explicit(Type type,Object value,Object fallback,boolean soft) {return Client.<Object>call(type,value,fallback,soft);}
}
