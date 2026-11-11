package fixture;
import java.util.List;
public final class Client {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static boolean call(List values,Predicate predicate) { return Provider.remove(values,predicate); }
}
