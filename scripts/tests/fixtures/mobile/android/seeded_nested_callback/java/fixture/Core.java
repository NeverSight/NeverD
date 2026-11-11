package fixture;
import java.util.List;
public final class Core {
    @SuppressWarnings("unchecked") public static List call(List values,Object seed,Context context,Sink sink){return Provider.adapt(values,seed,context,sink);}
    public static List checked(Object values,Object seed,Context context,Sink sink){return call((List)values,seed,context,sink);}
}
