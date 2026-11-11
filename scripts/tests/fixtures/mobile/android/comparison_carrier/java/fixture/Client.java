package fixture;
import java.util.List;
public final class Client {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static int call(List values,int from,int to,Comparison comparison) {
        return Provider.scan(values,from,to,comparison);
    }
}
