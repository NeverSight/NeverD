import fixture.Client;
import java.io.IOException;
import java.util.List;

public final class BoundedScalarTransferCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static List call(List values, int first, int second, boolean flag) throws IOException {
        return Client.call(values,first,second,flag);
    }
}
