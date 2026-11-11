package fixture;

import java.io.IOException;

public class Base {
    protected Class<?> read(Input input) throws ClassNotFoundException, IOException {
        Trace.hit(2, this, input, null);
        Class<?> result = input.token;
        Trace.hit(3, this, input, result);
        return result;
    }
}
