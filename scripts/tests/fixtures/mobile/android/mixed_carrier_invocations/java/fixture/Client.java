package fixture;
import fixture.api.Facade;
public final class Client {
    public static Object call(Object[] inputs, Saver<?,?> saver, Factory<?> producer, long flags) {
        return Facade.make(inputs, (Saver)saver, producer, flags);
    }
}
