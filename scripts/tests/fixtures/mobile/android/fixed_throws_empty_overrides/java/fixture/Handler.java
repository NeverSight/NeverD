package fixture;

public interface Handler {
    Object invoke(Object receiver, java.lang.reflect.Method method, Object[] arguments) throws Throwable;
}
