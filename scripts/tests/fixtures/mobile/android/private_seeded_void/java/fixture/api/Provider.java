package fixture.api;
import fixture.*;
class Provider {
    static { Trace.hit(1); }
    private static <T> void consume(T seed, Fn<? super T, String> callback, Factory<String> task, long flags) {
        Trace.flags = flags; Trace.hit(2);
        String saved = callback.apply(seed);
        Trace.saved = saved; Trace.hit(4);
        String result = task.read();
        Trace.taskResult = result; Trace.hit(6);
    }
    public static <T> void make(T seed, Fn<? super T,String> callback,Factory<String> task,long flags) { consume(seed,callback,task,flags); }
}
