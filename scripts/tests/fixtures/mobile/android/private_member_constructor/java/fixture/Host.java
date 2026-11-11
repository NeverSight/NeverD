package fixture;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.List;
import java.util.Objects;

public final class Host {
    private Host() {}
    public abstract static class Callback {
        public Callback() {}
        public abstract void fire(int code);
    }
    private static class State implements Runnable {
        List<Callback> callbacks;
        int code;
        Throwable problem;
        Callback original;
        State(Callback callback,int code) {
            this(Arrays.asList((Callback)Objects.requireNonNull(callback,"owned-callback")),code,null);
        }
        State(Collection<Callback> data,int code) {
            this(data,code,null);
        }
        State(Collection<Callback> data,int code,Throwable problem) {
            Trace.hit(1,this,data,code,problem);
            callbacks=new ArrayList<Callback>(Objects.requireNonNull(data,"owned-data"));
            this.code=code;
            this.problem=problem;
            Trace.hit(2,this,data,code,problem);
        }
        public void run() {
            Trace.hit(5,this,callbacks,code,problem);
            callbacks.get(0).fire(code);
        }
    }
    public static Runnable single(Callback callback,int code) {
        State value=new State(callback,code);value.original=callback;Trace.hit(4,value,null,code,null);return value;
    }
    public static Runnable group(Collection<Callback> data,int code) {
        State value=new State(data,code);Trace.hit(3,value,data,code,null);return value;
    }
    public static Runnable explicit(Collection<Callback> data,int code,Throwable problem) {return new State(data,code,problem);}
    public static List<Callback> values(Runnable value) {return ((State)value).callbacks;}
    public static Callback original(Runnable value) {return ((State)value).original;}
    public static int code(Runnable value) {return ((State)value).code;}
    public static Throwable problem(Runnable value) {return ((State)value).problem;}
}
