package fixture;
import java.util.Map;
import java.lang.reflect.Type;
public final class Trace {
    private Trace() {}
    public static Object source, tag, marker, nextSource;
    public static Object target;
    public static Map nextTarget;
    public static Config config, nextConfig;
    public static Type type, nextType;
    public static int bases, children, stage;
}
