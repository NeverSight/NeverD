package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Caller {
    public static boolean erased(Root self, Object value) { return self.matches(value); }
    public static boolean typed(Core self, Bound value) { return self.matches(value); }
    public static Object copy(Root self) { return self.copy(); }
}
