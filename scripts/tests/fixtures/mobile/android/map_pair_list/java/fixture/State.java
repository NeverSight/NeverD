package fixture;
import java.util.List;
import java.util.Map;
public class State {
    public static int events;
    public static Map savedInput;
    public static List replacement, savedResult;
    public static Object marker;
    public static Probe probe;
    public static RuntimeException failure;
}
