package fixture;
public class Trace {
    public static Core partial;
    public static long events;
    public static int names,fail;
    public static String label,changedLabel;
    public static Throwable failure;
    public static String nameValue() {
        names++;String snapshot=label;step(1);return snapshot;
    }
    public static void step(int tag) {
        events=events*10+tag;
        if(tag==1)label=changedLabel;
        if(fail==tag) {
            if(failure instanceof RuntimeException)throw (RuntimeException)failure;
            throw (Error)failure;
        }
    }
}
