import fixture.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ScopedRawInterfaceCaller {
    public static boolean erased(Root self, Object value) { return self.matches(value); }
    public static Object copy(Root self) { return self.copy(); }
}
