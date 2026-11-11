package fixture;
import api.*;
public class Trace {
    public static long events;
    public static int failAt,flags,mask;
    public static boolean done;
    public static Context context;
    public static RuntimeException failure;
    public static Object factory,first,reset,release,update,second,factorySeen,factoryResult,created,resetInput,releaseInput,updateInput;
    public static Unit resetResult,releaseResult,updateResult;
    public static void hit(int stage) { events=events*100+stage;if(stage==failAt)throw failure; }
}
