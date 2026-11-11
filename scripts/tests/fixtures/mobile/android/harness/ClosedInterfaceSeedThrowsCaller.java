import fixture.Client;
import fixture.Provider;
import java.util.Collection;
public final class ClosedInterfaceSeedThrowsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Collection call(Provider receiver,Collection values) throws fixture.Failure {return Client.call(receiver,values);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Collection full(Provider receiver,Collection values,boolean flag) throws fixture.Failure {return Client.full(receiver,values,flag);}
}
