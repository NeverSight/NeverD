package fixture;
import fixture.api.Facade;
public final class Client {
    public static Box<?> call(Box<?> seed, Fn<?,?> callback, long flags) {
        return Facade.make((Box)seed, (Fn)callback, flags);
    }
}
