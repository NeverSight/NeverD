import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class NestedFixedCallbackHarness {
    static int events,getters,calls;
    static Object seen,token;
    static RuntimeException failure;
    static final Unit SINGLE=new Unit();
    static void check(boolean ok) {
        if(!ok)throw new AssertionError("owned nested fixed callback behavior");
    }
    static final class Read implements Carrier<Object> {
        final Object value;final boolean fail;
        Read(Object value,boolean fail) { this.value=value;this.fail=fail; }
        public Object get() { events=events*10+1;getters++;if(fail)throw failure;return value; }
    }
    static final class Invoke implements Callback<Object,Unit> {
        final Unit value;final boolean fail;
        Invoke(Unit value,boolean fail) { this.value=value;this.fail=fail; }
        public Unit apply(Object input) { events=events*10+2;calls++;seen=input;if(fail)throw failure;return value; }
    }
    static Callback call(int route,Carrier state) throws Throwable {
        if(route==0)return Client.call(state);
        if(route==1)return NestedFixedCallbackCaller.call(state);
        if(route==2||route==3) {
            Method m=route==2?Provider.class.getMethod("access",Carrier.class):Provider.class.getDeclaredMethod("read",Carrier.class);
            if(route==3)m.setAccessible(true);
            try { return (Callback)m.invoke(null,state); } catch(InvocationTargetException e) { throw e.getCause(); }
        }
        if(route==4) {
            MethodHandle h=MethodHandles.lookup().findStatic(Provider.class,"access",MethodType.methodType(Callback.class,Carrier.class));
            return (Callback)h.invokeExact(state);
        }
        throw new AssertionError();
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            Unit unit=mode==6?null:mode==8?new Unit():SINGLE;
            Callback expected=new Invoke(unit,mode==5);
            if(mode==9)expected=new Invoke(new Unit(),false);
            Object stored=mode==2?new Object():mode==3?null:expected;
            Carrier state=mode==1?null:new Read(stored,mode==4);
            Provider.replacement=new Invoke(SINGLE,false);
            token=mode==7?null:new Object();seen=null;events=0;getters=0;calls=0;
            failure=new RuntimeException("owned failure");Callback callback=null;Object result=null;Throwable caught=null;
            try { callback=call(route,state);if(callback!=null)result=callback.apply(token); }
            catch(Throwable e) { caught=e; }
            check(getters==(mode==1?0:1));
            check(events==(mode==1?0:mode==2||mode==3||mode==4?1:12));
            check(calls==(mode==1||mode==2||mode==3||mode==4?0:1));
            if(mode==1)check(caught instanceof NullPointerException);
            else if(mode==2)check(caught instanceof ClassCastException);
            else if(mode==4||mode==5)check(caught==failure);
            else check(caught==null);
            check(callback==(mode==1||mode==2||mode==3||mode==4?null:expected));
            if(calls!=0)check(seen==token);
            if(calls!=0&&mode!=5)check(result==((Invoke)expected).value);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
