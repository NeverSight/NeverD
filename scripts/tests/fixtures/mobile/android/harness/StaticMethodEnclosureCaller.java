import fixture.Provider;
public class StaticMethodEnclosureCaller {
    public static Object call(Object input, Object marker, boolean defaults) {
        return defaults ? Provider.relay(input) : Provider.provided(input, marker);
    }
}
