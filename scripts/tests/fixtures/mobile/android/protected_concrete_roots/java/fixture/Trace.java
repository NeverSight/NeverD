package fixture;
public final class Trace {
    public static int calls, phase, tag, fail;
    public static Object receiver;
    public static long bits;
    public static Throwable failure;
    public static void before(Object self,long value,int which) throws Throwable {
        ++calls;receiver=self;bits=value;tag=which;phase=1;
        if(fail==1)throw failure;
    }
    public static void after() throws Throwable {
        phase=2;if(fail==2)throw failure;
    }
}
