import fixture.Client;
import java.util.Map;
public final class CarrierNominalVarargsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String call(Map values,String first,String second){return Client.call(values,first,second);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String full(Map values,String first,String second,String[] tail){return Client.full(values,first,second,tail);}
}
