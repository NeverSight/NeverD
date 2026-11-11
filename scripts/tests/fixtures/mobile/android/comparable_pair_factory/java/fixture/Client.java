package fixture;
import java.util.Map;
import java.util.Comparator;
public class Client {
    public static <K,V extends Comparable<? super V>> Comparator<Map.Entry<K,V>> call(){return Provider.select();}
    public static <K,V extends Comparable<? super V>> Comparator<Map.Entry<K,V>> full(){return Provider.standard();}
}
