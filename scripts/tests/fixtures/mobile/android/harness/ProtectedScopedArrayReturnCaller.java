package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedScopedArrayReturnCaller {
    public static Object erased(Base self, Object input) { return self.dispatch(input); }
    public static <E> E[] typed(Core<E> self, Result<E> input) { return self.convert(input); }
}
