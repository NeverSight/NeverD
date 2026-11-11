import fixture.Client;
import java.util.Map;
import java.util.Comparator;
import java.util.TreeMap;
public final class PairedKeyConsumerCaller {
    @SuppressWarnings("unchecked") public static TreeMap call(Map source){return Client.call(source);}
    @SuppressWarnings("unchecked") public static TreeMap full(Map source,Comparator comparator){return Client.full(source,comparator);}
}
