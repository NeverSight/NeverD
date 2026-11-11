package fixture;
import java.lang.reflect.Type;
public class Client {
    public static <E> E call(Type type,Object value,E fallback) {return call(type,value,fallback,false);}
    public static <E> E call(Type type,Object value,E fallback,boolean soft) {
        try {return Trace.provider.convert(type,value,fallback);}
        catch(Exception failure) {if(soft)return fallback;throw failure;}
    }
}
