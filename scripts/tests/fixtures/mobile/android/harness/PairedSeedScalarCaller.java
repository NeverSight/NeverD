import fixture.Client;
import java.util.Map;
public final class PairedSeedScalarCaller {
    public static Map.Entry call(Object key,Object value){return Client.call(key,value);}
    public static Map.Entry full(Object key,Object value,boolean immutable){return Client.full(key,value,immutable);}
}
