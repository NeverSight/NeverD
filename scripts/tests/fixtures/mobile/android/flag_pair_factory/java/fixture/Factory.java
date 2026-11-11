package fixture;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public class Factory {
    public static <K,V> HashMap<K,V> make(boolean flag) {return make(73,flag);}
    public static <K,V> HashMap<K,V> make(int capacity,boolean flag) {
        Trace.calls++;Trace.capacitySeen=capacity;Trace.flagSeen=flag;Trace.stage=1;
        Trace.action.run();Trace.stage=2;return (HashMap<K,V>)Trace.result;
    }
}
