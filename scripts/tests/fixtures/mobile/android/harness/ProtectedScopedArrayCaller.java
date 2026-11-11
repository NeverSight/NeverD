package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedScopedArrayCaller {
    public static Object erased(Base self, Object input) { return self.dispatch(input); }
    public static <E> Result<E> typed(Core<E> self, E[] input) { return self.convert(input); }
}
