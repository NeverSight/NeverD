package fixture;
public final class Trace {
    private Trace() {}
    public static long events;
    public static int fail,hint;
    public static boolean mutate;
    public static Throwable failure;
    public static Object seed,next,firstValue,secondValue;
    public static Policy factoryPolicy;
    public static Core escaped;
    public static Policy policy5,policy6;
    public static Object first5,first6,second5,second6;
    public static void step(int value) {
        events=events*10+value;
        if(fail==value) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
        if(value==3&&mutate)Factory.swap(next);
    }
    public static Object produce(int value) {
        if(value==5){policy5=escaped.policy;first5=escaped.first;second5=escaped.second;}
        else {policy6=escaped.policy;first6=escaped.first;second6=escaped.second;}
        step(value);return value==5?firstValue:secondValue;
    }
}
