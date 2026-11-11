package fixture;

import java.io.IOException;
import java.security.Key;

public class Client {
    public static <T extends CharSequence> T call(Provider receiver, Key context, Class<T> type) throws IOException {
        return receiver.read(context,type);
    }
}
