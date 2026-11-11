package fixture;
import fixture.api.Facade;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Object call(Enum[] values) {
        Box<?> result = (Box) Facade.make(values);
        Trace.after = result; Trace.hit(5);
        return result;
    }
    public static Thread.State checked(Enum[] values) {
        Box result = (Box) Facade.make(values);
        Trace.after = result; Trace.hit(5);
        return (Thread.State) result.value;
    }
}
