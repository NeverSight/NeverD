import fixture.Client;
import fixture.Provider;
import fixture.Handler;
public final class ProducerThrowsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Object call(Provider receiver,Handler handler){return Client.call(receiver,handler);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Object ignore(Provider receiver,Handler handler){return Client.ignore(receiver,handler);}
}
