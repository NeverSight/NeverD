package fixture;
import java.util.Map;
public class Client {
    public static <K,V> String call(Map<K,V> values,String first,String second) throws Failure {return Provider.select(values,first,second);}
    public static <K,V> String full(Map<K,V> values,String first,String second,String[] tail) throws Failure {return Provider.full(values,first,second,tail);}
}
