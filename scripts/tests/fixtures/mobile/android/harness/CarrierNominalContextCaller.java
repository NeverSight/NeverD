import fixture.Client;
import java.io.File;
import java.util.Collection;
public final class CarrierNominalContextCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static File call(Collection values,File file,String text){return Client.call(values,file,text);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static File full(Collection values,File file,String text,boolean flag){return Client.full(values,file,text,flag);}
}
