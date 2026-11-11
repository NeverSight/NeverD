import fixture.Client;
import fixture.Provider;
import java.util.Collection;
public final class ParameterizedInterfaceSeedCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Collection call(Provider receiver,Collection values){return Client.call(receiver,values);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Collection full(Provider receiver,Collection values,boolean flag){return Client.full(receiver,values,flag);}
}
