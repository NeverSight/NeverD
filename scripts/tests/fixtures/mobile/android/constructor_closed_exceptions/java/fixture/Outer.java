package fixture;

import java.io.IOException;

public class Outer {
    public Object captured;

    public Outer(Object captured) { this.captured = captured; }

    public Base make(Trace trace, Object value) throws IOException {
        trace.hit(0, this, value);
        Base result = new Base(trace, value) {
            @Override public Object value() { return Outer.this.captured; }
        };
        trace.hit(3, this, result);
        return result;
    }

    public static <T> T identity(T value) { return value; }
}
