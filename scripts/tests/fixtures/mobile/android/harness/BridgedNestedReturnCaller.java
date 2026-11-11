package fixture;
import java.util.Iterator;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class BridgedNestedReturnCaller {
    public static Iterator call(Base self, Object value) { return self.dispatch(value); }
    public static Iterator typed(Core self, Token value) { return self.values(value); }
}
