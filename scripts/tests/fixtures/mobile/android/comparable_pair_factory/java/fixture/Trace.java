package fixture;
import java.util.Comparator;
public class Trace {
    public static int calls,path,leftReads,rightReads,compares,failAt;
    public static long events;
    public static Object payload,comparePayload,result;
    public static Comparator platform;
    public static Runnable action;
    public static Failure failure;
    public static void hit(int stage){events=events*10+stage;if(stage==failAt)throw failure;}
}
