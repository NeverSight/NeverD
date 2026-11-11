import fixture.Client;
import java.lang.reflect.Type;
public final class TokenSeedUncheckedCaller {
    @SuppressWarnings("unchecked") public static Object call(Class type,Object value,Object fallback){return Client.call(type,value,fallback);}
    public static Object full(Type type,Object value,Object fallback){return Client.full(type,value,fallback);}
}
