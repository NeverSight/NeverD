import fixture.Client;
import fixture.Provider;
import java.io.File;
import java.lang.Iterable;
public final class InstanceNominalThrowsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static File call(Provider receiver,Iterable values,File file,Thread.State context){return Client.call(receiver,values,file,context);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static File full(Provider receiver,Iterable values,File file,Thread.State context,boolean flag){return Client.full(receiver,values,file,context,flag);}
}
