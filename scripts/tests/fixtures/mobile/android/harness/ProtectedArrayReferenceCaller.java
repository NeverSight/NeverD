package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedArrayReferenceCaller {
    public static Object erased(Base self, Object input) { return self.dispatch(input); }
    public static Result typed(Core self, boolean[] input) { return self.convert(input); }
}
