import fixture.*;
public class ProjectedSeedCarrierCaller {
    public static void call(Object carrier, Object value, Callback<Object,Object,Object> callback) {
        Core.call(carrier, value, callback);
    }
}
