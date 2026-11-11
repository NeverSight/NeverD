package fixture;
import java.util.Calendar;
public class Trace {
    public static int calls,failAt;
    public static long events;
    public static Object receiver,payload;
    public static Calendar calendar;
    public static Appendable value,result;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
