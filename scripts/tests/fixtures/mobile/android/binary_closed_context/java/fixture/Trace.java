package fixture;
public class Trace {
    public static int calls,failAt,inputCalls,mapperCalls,outputCalls;
    public static long events;
    public static Object input,context,result,payload,inputArg,mapArg,outputArg;
    public static Runnable action;
    public static RuntimeException failure;
    public static void hit(int event){events=events*10+event;if(failAt==event)throw failure;}
}
