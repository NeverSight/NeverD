package fixture;
public class Trace {
    public static Object partial,firstSeen,secondSeen,seen,result;
    public static long events;
    public static int tags,fail;
    public static Throwable failure;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure;}
    }
    public static Object accept(int tag,Object value) {
        tags=tags*10+tag;step(5);seen=value;step(6);return result;
    }
}
