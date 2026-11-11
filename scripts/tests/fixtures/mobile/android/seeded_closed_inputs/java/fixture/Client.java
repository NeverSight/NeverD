package fixture;
import fixture.api.Facade;
public final class Client {
    public static Box<?> call(Object seed, Box<?> input, Fn<?,?> closed,
            Fn<?,?> callback, Producer<?> producer) {
        return Facade.make(seed, (Box)input, (Fn)closed, (Fn)callback, (Producer)producer);
    }
}
