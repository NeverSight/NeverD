package fixture;
import java.util.Comparator;
public final class Core {
    private Core() {}
    public static Subject<Object> simple(int number) {return new Subject<Object>(number);}
    public static Subject<Object> direct(int number,Comparator<Object> callback) {return new Subject<Object>(number,callback);}
    public static Subject<String> typed(int number) {return new Subject<String>(number);}
    public static Object ignored(int number) {new Subject<Object>(number);return Trace.marker;}
    public static Subject<Object> directNull(int number) {return new Subject<Object>(number,(Comparator<Object>)null);}
    public static Subject<CharSequence> other(int number) {return new Subject<CharSequence>(number);}
}
