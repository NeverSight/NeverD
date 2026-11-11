package fixture;
public final class Trace {
    public static Core partial;
    public static RuntimeException failure;
    public static int failAt,position,count,stepCalls;
    public static long events;
    public static Object last;
    public static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    public static boolean next(Iterable<?> input){if(input==null)throw new NullPointerException();hit(1);return position<count;}
    public static Object step(Iterable<?> input){hit(2);stepCalls++;Object result=input.iterator().next();position++;last=result;return result;}
}
