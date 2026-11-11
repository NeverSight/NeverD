package fixture;
import java.util.Map;
public class Client {
    public static <K,V> String call(Map<K,V> values,String first,String second,boolean flag) throws Failure {return Provider.select(values,first,second,flag);}
    public static <K,V> String full(Map<K,V> values,String first,String second,boolean flag,String[] tail) throws Failure {return Provider.full(values,first,second,flag,tail);}
}
