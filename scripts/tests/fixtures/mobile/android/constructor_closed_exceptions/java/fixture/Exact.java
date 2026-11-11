package fixture;

import java.io.IOException;

public class Exact extends Base {
    public Exact(Trace trace, Object value) throws IOException, IllegalArgumentException {
        super(trace, value);
        trace.hit(4, this, value);
    }

    public Exact(Trace trace, Object value, int ignored) {
        super(trace, value, ignored);
        trace.unchecked(7, this, value);
    }
}
