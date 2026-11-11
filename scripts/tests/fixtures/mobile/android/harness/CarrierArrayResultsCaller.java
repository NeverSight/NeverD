import fixture.*;
public final class CarrierArrayResultsCaller {
    public static String[] call(Facade self, int begin, int end, Class<?> token) {
        return Core.read(self, begin, end, token);
    }
}
