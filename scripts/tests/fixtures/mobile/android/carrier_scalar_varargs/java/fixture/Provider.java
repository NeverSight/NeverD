package fixture;
import java.util.Map;
public class Provider {
    public static <K,V> String select(Map<K,V> values,String first,String second,boolean flag) throws Failure {return full(values,first,second,flag,new String[0]);}
    public static <K,V> String full(Map<K,V> values,String first,String second,boolean flag,String... tail) throws Failure {Trace.calls++;Trace.values=values;Trace.first=first;Trace.second=second;Trace.flag=flag;Trace.tail=tail;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
