import fixture.Client;
import fixture.Provider;
import java.io.IOException;
import java.security.Key;

public final class InstanceBoundTokenCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static CharSequence call(Provider receiver, Key context, Class type) throws IOException {
        return Client.call(receiver,context,type);
    }
}
