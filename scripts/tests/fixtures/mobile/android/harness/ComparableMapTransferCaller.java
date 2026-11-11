import fixture.Core;
import java.util.Map;
public final class ComparableMapTransferCaller {
    public static Map call(Map input,boolean reverse){return Core.call(input,reverse);}
    public static Map checked(Object input,boolean reverse){return Core.checked(input,reverse);}
}
