import fixture.Client;
import java.util.Map;
public final class CarrierScalarVarargsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String call(Map values,String first,String second,boolean flag){return Client.call(values,first,second,flag);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static String full(Map values,String first,String second,boolean flag,String[] tail){return Client.full(values,first,second,flag,tail);}
}
