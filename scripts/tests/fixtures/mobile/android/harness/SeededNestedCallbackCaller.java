import fixture.*;
import java.util.List;
public final class SeededNestedCallbackCaller {
    public static List call(List values,Object seed,Context context,Sink sink){return Core.call(values,seed,context,sink);}
    public static List checked(Object values,Object seed,Context context,Sink sink){return Core.checked(values,seed,context,sink);}
}
