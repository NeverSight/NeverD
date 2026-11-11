package fixture;

import java.io.IOException;

public class Host {
    public Class<?> fallback;
    public int calls;

    public Base make() {
        return new Base() {
            @Override protected Class<?> read(Input input) throws IOException, ClassNotFoundException {
                Host.this.calls++;
                Trace.hit(1, this, input, Host.this);
                if (input == null) {
                    Class<?> fallback = Host.this.fallback;
                    Trace.hit(5, this, input, fallback);
                    return fallback;
                }
                Class<?> result = super.read(input);
                Trace.hit(4, this, input, result);
                return result;
            }
        };
    }
}
