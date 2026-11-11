package fixture;
import java.lang.reflect.Type;
public class Client {
    public static <T> T call(Class<T> type,Object value,T fallback)throws Failure{return Provider.convert(type,value,fallback);}
    public static <T> T full(Type type,Object value,T fallback)throws Failure{return Provider.opaque(type,value,fallback);}
}
