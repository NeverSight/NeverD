package fixture;

import java.io.IOException;

public abstract class Trace {
    public Base parent;
    public abstract void hit(int phase, Object self, Object value) throws IOException;
    public abstract void unchecked(int phase, Object self, Object value);
}
