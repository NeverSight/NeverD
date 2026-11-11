import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedConverterHarness {
    static int events,getters,calls;
    static Object seen,token;
    static Callback callbackSeen;
    static RuntimeException failure;
    static void check(boolean ok) {
        if(!ok)throw new AssertionError("owned bounded converter behavior");
    }
    static final class Derived extends Bound { Derived(int value) { super(value); } }
    static final class Read implements Converter {
        final Callback callback;final boolean fail;
        Read(Callback callback,boolean fail) { this.callback=callback;this.fail=fail; }
        public Callback get() { events=events*10+1;getters++;if(fail)throw failure;return callback; }
    }
    static final class Invoke implements Callback {
        final Object value;final boolean fail;
        Invoke(Object value,boolean fail) { this.value=value;this.fail=fail; }
        public Object apply(Object input) {
            check(input==token);events=events*10+2;calls++;seen=input;callbackSeen=this;
            if(fail)throw failure;return value;
        }
    }
    static Bound call(int route,Converter converter,Object value) throws Throwable {
        if(route==0)return Client.call(converter,value);
        if(route==1)return BoundedConverterCaller.call(converter,value);
        if(route==2||route==3) {
            Method m=route==2?Provider.class.getMethod("access",Converter.class,Object.class)
                :Provider.class.getDeclaredMethod("convert",Converter.class,Object.class);
            if(route==3)m.setAccessible(true);
            try { return (Bound)m.invoke(null,converter,value); } catch(InvocationTargetException e) { throw e.getCause(); }
        }
        if(route==4) {
            MethodHandle h=MethodHandles.lookup().findStatic(Provider.class,"access",MethodType.methodType(Bound.class,Converter.class,Object.class));
            return (Bound)h.invokeExact(converter,value);
        }
        throw new AssertionError();
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            token=mode==1||mode==2?null:mode==10?new Bound(17):new Object();
            Object expected=mode==7?new Object():mode==8?null:mode==10?token
                :mode==11?new Derived(31+attempt):new Bound(mode==12?Integer.MIN_VALUE:31+attempt);
            Callback callback=mode==5?null:new Invoke(expected,mode==6);
            if(mode==9)callback=new Invoke(expected,false);
            Converter converter=mode==2||mode==3?null:new Read(callback,mode==4);
            Provider.replacement=new Bound(-1);seen=null;callbackSeen=null;events=0;getters=0;calls=0;
            failure=new RuntimeException("owned failure");Bound result=null;Throwable caught=null;
            try { result=call(route,converter,token); } catch(Throwable e) { caught=e; }
            boolean skipped=mode==1||mode==2;
            check(getters==(skipped||mode==3?0:1));
            check(events==(skipped||mode==3?0:mode==4||mode==5?1:12));
            check(calls==(skipped||mode==3||mode==4||mode==5?0:1));
            if(mode==3||mode==5)check(caught instanceof NullPointerException);
            else if(mode==7)check(caught instanceof ClassCastException);
            else if(mode==4||mode==6)check(caught==failure);
            else check(caught==null&&result==(skipped?null:expected));
            if(calls!=0)check(seen==token&&callbackSeen==callback);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
