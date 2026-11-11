package fixture;
public class Trace {
    public static int factories,failAt,givenIndex;
    public static long events;
    public static Object inputLeft,inputRight,receiver,adapter,callbackFirst,callbackSecond,payload,result;
    public static String left,right;
    public static Runnable baseAction,callbackAction;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
