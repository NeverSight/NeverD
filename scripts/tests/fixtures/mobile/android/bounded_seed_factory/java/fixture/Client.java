package fixture;
import java.util.List;
public class Client {
    @SuppressWarnings("unchecked")
    public static List call(Object context,int index,List values,Object extra,long bits,Fn factory) {
        return Provider.fill(context,index,values,extra,bits,factory);
    }
}
