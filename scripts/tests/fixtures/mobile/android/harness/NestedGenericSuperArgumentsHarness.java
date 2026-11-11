import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public class NestedGenericSuperArgumentsHarness {
    public static class Future extends Caller<Bound> {
        public Object read(Context context, Bound input, long bits) {
            Trace.hit(9); Object result=super.read(context,input,bits); Trace.hit(0); return result;
        }
    }
    static void check(boolean value) {
        if(!value) throw new AssertionError("owned nested generic super arguments mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable error) throws E { throw (E)error; }
    static Object invoke(Caller<Bound> self,int route,Context context,Bound input,long bits) throws Throwable {
        if(route==0) return self.read(context,input,bits);
        if(route==1) return NestedGenericSuperArgumentsCaller.call(self,context,input,bits);
        if(route==2) {
            try { return Caller.class.getMethod("read",Context.class,Bound.class,long.class).invoke(self,context,input,bits); }
            catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle call=MethodHandles.publicLookup().findVirtual(Caller.class,"read",MethodType.methodType(Object.class,Context.class,Bound.class,long.class));
        return (Object)call.invokeExact((Caller)self,context,input,bits);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), future=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        final Caller<Bound> self=future==0 ? new Caller<Bound>() : new Future();
        for(int attempt=0;attempt<2;++attempt) {
            final Context context=attempt==0 && mode!=11 ? new Context() : null;
            final Bound input=attempt==0 && mode!=11 ? new Bound() : null;
            final long bits=attempt==0 ? 0x12345678fedcba98L : Long.MIN_VALUE+31;
            final Object reply=mode==10 ? null : new Object();
            final Throwable failure=mode==2 || mode==5 ? new AssertionError() : mode==6 ? new Exception() : new IllegalStateException();
            final int currentMode=mode;
            final long prefix=(future==0 ? 0 : 9)*10+(attempt==0 ? 1 : 0);
            final long callbackPrefix=(attempt==0 ? prefix : (future==0 ? 0 : 9))*100+23;
            Trace.events=0; Trace.bits=0; Trace.failAt=mode==1 || mode==2 ? 1 : mode==3 ? 2 : mode==7 ? 6 : mode==8 ? 7 : mode==9 ? 8 : -1;
            Trace.receiver=null; Trace.context=null; Trace.input=null; Trace.wrapper=null; Trace.reply=new Object();
            Trace.failure=new Runnable() { public void run() { NestedGenericSuperArgumentsHarness.<RuntimeException>raise(failure); }};
            Trace.action=new Runnable() { public void run() {
                check(Trace.events==callbackPrefix && Trace.receiver==self && Trace.context==context && Trace.input==input && Trace.bits==bits);
                Trace.hit(5); Trace.reply=reply; Trace.receiver=new Object(); Trace.context=new Object();
                if(input!=null) input.mark=19;
                if(currentMode>=4 && currentMode<=6) NestedGenericSuperArgumentsHarness.<RuntimeException>raise(failure);
            }};
            Object result=null; Throwable caught=null;
            try { result=invoke(self,route,context,input,bits); } catch(Throwable error) { caught=error; }
            long expected=future==0 ? 0 : 9;
            if(mode==1 || mode==2) {
                if(attempt==0) {
                    expected=expected*10+1;
                    if(mode==2) check(caught==failure);
                    else check(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
                } else check(caught instanceof NoClassDefFoundError);
                check(Trace.wrapper==null && Trace.receiver==null);
            } else {
                if(attempt==0) expected=expected*10+1;
                int stop=mode==3 ? 2 : mode>=4 && mode<=6 ? 5 : mode==7 ? 6 : mode==8 ? 7 : mode==9 ? 8 : -1;
                for(int event:new int[]{2,3,5,4,6,7,8}) {
                    expected=expected*10+event;
                    if(event==stop) break;
                }
                if(stop<0) {
                    if(future!=0) expected=expected*10;
                    check(caught==null && result==Trace.wrapper && result instanceof Result);
                    Result wrapped=(Result)result;
                    check(wrapped.original==reply && wrapped.owner==self);
                } else {
                    check(caught==failure);
                    if(stop==7 || stop==8) check(Trace.wrapper!=null && Trace.wrapper.original==reply && Trace.wrapper.owner==(stop==8 ? self : null));
                    else check(Trace.wrapper==null);
                }
                if(input!=null && stop!=2) check(input.mark==19);
            }
            check(Trace.events==expected);
            System.out.println("route="+route+",future="+future+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
