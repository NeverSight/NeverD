package fixture;
import java.util.List;
public final class Core {
    @SuppressWarnings("unchecked") public static List call(List values,Object seed){return Provider.transfer(values,seed);}
    public static List checked(Object values,Object seed){return call((List)values,seed);}
}
