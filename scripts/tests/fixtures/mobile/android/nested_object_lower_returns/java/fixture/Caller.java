package fixture;
import java.util.Iterator;
public final class Caller {
    private Caller() {}
    public static Iterator<?> call(View value) { return value.values(); }
}
