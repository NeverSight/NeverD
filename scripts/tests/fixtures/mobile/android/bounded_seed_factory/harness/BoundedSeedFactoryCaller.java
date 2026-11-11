import fixture.*;
import java.util.List;
public class BoundedSeedFactoryCaller {
    @SuppressWarnings("unchecked")
    public static List call(Object context,int index,List values,Object extra,long bits,Fn factory) {
        return Provider.fill(context,index,values,extra,bits,factory);
    }
}
