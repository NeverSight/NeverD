package fixture;
import fixture.api.Facade;
public final class Client {
    public static Box<?> call(Fn<?,?,?> callback, long flags) {
        return Facade.make((Fn)callback, flags);
    }
}
