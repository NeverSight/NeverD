import fixture.*;
import java.util.List;
public final class ComparisonCarrierCaller {
    public static int call(List values,int from,int to,Comparison comparison) {
        return Client.call(values,from,to,comparison);
    }
}
