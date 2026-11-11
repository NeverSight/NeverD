import fixture.*;
public final class CarrierElementCaller {
    public static Object element(Factory<?> factory, long flags) { return Client.element(factory, flags); }
    public static String text(Factory<?> factory, long flags) { return Client.text(factory, flags); }
}
