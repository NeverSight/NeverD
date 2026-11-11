package fixture;
import java.io.IOException;

public final class Trace {
    public static Probe probe;
    public static Object[] capture(Object[] values) throws IOException {
        probe.allocated(values);
        return values;
    }
}
