package fixture;
import fixture.api.Facade;
public final class Client {
    public static Carrier<?> call(Carrier<?> input) {
        Carrier<?> result = Facade.relay((Carrier) input);
        Trace.after = result;
        return result;
    }
}
