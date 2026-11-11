import fixture.Client;
import fixture.Provider;
import fixture.Value;
public final class ClosedClassSeedThrowsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Value call(Provider receiver,Value values) throws fixture.Failure {return Client.call(receiver,values);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Value full(Provider receiver,Value values,long count,boolean flag) throws fixture.Failure {return Client.full(receiver,values,count,flag);}
}
