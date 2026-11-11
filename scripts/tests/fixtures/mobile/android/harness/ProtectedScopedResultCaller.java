package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedScopedResultCaller {
    public static Object call(int kind, Core self, Box input) {
        switch (kind) {
        case 0: return self.make();
        case 1: return self.size(input);
        case 2: return self.copy(input);
        default: return self.narrow(input);
        }
    }
}
