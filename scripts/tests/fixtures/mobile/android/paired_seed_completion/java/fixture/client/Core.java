package fixture.client;
import fixture.*;
import fixture.api.Api;
public class Core {
    public static Object call(Fn<Object, Object, Completion<? super Object>, Object> callback,
                              Object left, Object right, Completion<Object> completion) {
        Trace.hit(6);
        Object result = Api.invoke(callback, left, right, completion);
        Trace.hit(0);
        return result;
    }
}
