package fixture;

import java.io.IOException;

public class Base {
    public Trace observer;
    public Object saved;

    public Base(Trace trace, Object value) throws IOException {
        trace.hit(1, this, value);
        this.observer = trace;
        this.saved = value;
        trace.parent = this;
        trace.hit(2, this, value);
    }

    public Base(Trace trace, Object value, int ignored) throws IllegalArgumentException {
        trace.unchecked(5, this, value);
        this.observer = trace;
        this.saved = value;
        trace.parent = this;
        trace.unchecked(6, this, value);
    }

    public Object value() { return saved; }
}
