import fixture.Client;
import java.util.Map;
import java.util.function.Function;
public final class PairedSeedElementCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String call(Map values,Object key,Function function){return (String)Client.call(values,key,function);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String full(Map values,Object key,Function function){return (String)Client.full(values,key,function);}
}
