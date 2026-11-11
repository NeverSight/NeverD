package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class InheritedProtectedProviderCaller {
    public static Box typed(Provider self, Box input) { return self.read(input); }
    public static Box erased(Base self, Object input) { return self.dispatch(input); }
}
