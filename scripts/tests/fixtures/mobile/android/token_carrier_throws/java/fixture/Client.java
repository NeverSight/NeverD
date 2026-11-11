package fixture;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.List;

public class Client {
    public static <T> List<T> call(ByteBuffer context, Class<T> token) throws IOException {
        return Provider.read(context,token);
    }
}
