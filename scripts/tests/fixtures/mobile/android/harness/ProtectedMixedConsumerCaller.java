package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedMixedConsumerCaller {
    public static void root(Base self, Token token, int index, Object value, boolean flag, long cookie, double weight) {
        self.dispatch(token, index, value, flag, cookie, weight);
    }
    public static void middle(Middle self, Token token, int index, Builder value, boolean flag, long cookie, double weight) {
        self.read(token, index, value, flag, cookie, weight);
    }
    public static void typed(Core self, Token token, int index, Special value, boolean flag, long cookie, double weight) {
        self.read(token, index, value, flag, cookie, weight);
    }
}
