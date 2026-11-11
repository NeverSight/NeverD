import fixture.Client;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.List;

public final class TokenCarrierThrowsCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static List call(ByteBuffer context, Class token) throws IOException {
        return Client.call(context,token);
    }
}
