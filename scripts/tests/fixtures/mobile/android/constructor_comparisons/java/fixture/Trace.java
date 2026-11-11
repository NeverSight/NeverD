package fixture;
public class Trace {
    public static long events,firstBits,secondBits;
    public static int calls,allocations,fail;
    public static Throwable failure;
    public static Core partial;
    public static Value created;
    public static Object identity;
    public static double first,second;
    public static void step(int tag) {
        events=events*10+tag;
        if (fail==tag) {
            if (failure instanceof RuntimeException) throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
    public static double choose(double input) {
        int index=++calls;
        double result=index==1 ? first : second;
        if (index==1) firstBits=Double.doubleToRawLongBits(input);
        else secondBits=Double.doubleToRawLongBits(input);
        step(index);
        return result;
    }
}
