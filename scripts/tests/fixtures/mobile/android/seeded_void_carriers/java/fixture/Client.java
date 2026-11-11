package fixture;
import fixture.api.Facade;
public final class Client {
    public static void call(Object seed, Fn<?,?> callback, Factory<?> task, long flags) {
        Facade.make(seed, (Fn) callback, (Factory) task, flags);
    }
}
