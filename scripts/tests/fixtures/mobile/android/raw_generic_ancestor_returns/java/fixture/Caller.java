package fixture;
import java.util.Iterator;
public final class Caller {
    public static Iterator<?> call(Core value) { return value.values(); }
}
