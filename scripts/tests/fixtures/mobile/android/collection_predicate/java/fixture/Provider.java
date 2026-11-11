package fixture;
import java.util.Iterator;
import java.util.List;
public final class Provider {
    public static <T> boolean remove(List<T> values, Predicate<? super T,? extends Boolean> predicate) {
        Trace.hit(1);
        boolean changed=false;
        Iterator<T> cursor=values.iterator();
        while(cursor.hasNext()) {
            T value=cursor.next(); Trace.current=value; Trace.hit(2);
            if(predicate.apply(value).booleanValue()) { Trace.hit(3); cursor.remove(); changed=true; }
            Trace.hit(4);
        }
        Trace.hit(5);
        return changed;
    }
}
