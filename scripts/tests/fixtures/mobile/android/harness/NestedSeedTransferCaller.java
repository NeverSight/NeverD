import fixture.Core;
import java.util.List;
public final class NestedSeedTransferCaller {
    public static List call(List values,Object seed){return Core.call(values,seed);}
    public static List checked(Object values,Object seed){return Core.checked(values,seed);}
}
