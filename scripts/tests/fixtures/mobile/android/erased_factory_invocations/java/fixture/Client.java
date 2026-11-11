package fixture;
import fixture.api.Facade;
public final class Client {
    public static Box<?> call(Factory<?> factory, long flags) {
        return Facade.make(factory, flags);
    }
    public static Object element(Factory<?> factory, long flags) {
        return Facade.element(factory, flags);
    }
    public static String text(Factory<?> factory, long flags) {
        return (String)Facade.element(factory, flags);
    }
    public static Box<?> seed(Object value, long flags) {
        return Facade.seed(value, flags);
    }
}
