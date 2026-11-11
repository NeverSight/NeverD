package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedScopedVoidCaller {
    public static void call(Base self, Object box, int index, Object value) { self.dispatch(box, index, value); }
    public static void typed(Core self, Box box, int index, Object value) { self.insert(box, index, value); }
}
