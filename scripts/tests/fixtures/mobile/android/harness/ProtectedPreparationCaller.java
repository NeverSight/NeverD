import fixture.*;
public final class ProtectedPreparationCaller {
    public static Object create(Object input) { return Maker.create(input); }
    public static Object raised(Object input) { return Maker.raised(input); }
}
