import fixture.Client;
import java.util.Map;
public final class PairedRawTokenContextCaller {
    public static Map<?,?> defaults(Class<String> keys,Class<Integer> values,Object value) {return Client.defaults(keys,values,value);}
    public static Map<?,?> explicit(Class<? extends Map> kind,Class<String> keys,Class<Integer> values,Object value) {return Client.explicit(kind,keys,values,value);}
}
