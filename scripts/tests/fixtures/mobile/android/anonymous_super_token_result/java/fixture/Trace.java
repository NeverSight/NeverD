package fixture;

import java.io.IOException;

public abstract class Trace {
    public static Trace current;
    public abstract void observe(int phase, Object owner, Input input, Object value)
            throws IOException, ClassNotFoundException;

    public static void hit(int phase, Object owner, Input input, Object value)
            throws IOException, ClassNotFoundException {
        if (current != null) current.observe(phase, owner, input, value);
    }
}
