package fixture;
import fixture.api.Facade;
public final class Client {
    public static Box<?> call(Object seed, Fn<?,?> callback, long flags) {
        return Facade.make(seed, (Fn)callback, flags);
    }
}
