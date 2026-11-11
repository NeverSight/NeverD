import fixture.Client;
import java.nio.ByteOrder;
public final class BoundedTokenPrimitiveArrayCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Number call(byte[] data,Class type,ByteOrder order){return Client.call(data,type,order);}
}
