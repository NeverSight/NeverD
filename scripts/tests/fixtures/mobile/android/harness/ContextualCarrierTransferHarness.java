package fixture;
import java.io.IOException;
import java.io.FileNotFoundException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ContextualCarrierTransferHarness {
    static final class Alien implements Carrier<Object> {}
    static void check(boolean value) { if(!value) throw new AssertionError("owned contextual carrier transfer mismatch"); }
    @SuppressWarnings("unchecked") static <E extends Throwable> void raise(Throwable e) throws E { throw (E)e; }
    static Carrier<?> invoke(int route,int operation,Context context,Carrier<?> seed,Class<?> token) throws Throwable {
        if(route==0) return operation==0 ? Core.call(context,seed,token) : Core.checked(context,seed,token);
        if(route==1) return ContextualCarrierTransferCaller.call(operation,context,seed,token);
        String name=operation==0 ? "call" : "checked";
        if(route==2) {
            try { return (Carrier<?>)Core.class.getMethod(name,Context.class,Carrier.class,Class.class).invoke(null,context,seed,token); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        return (Carrier<?>)MethodHandles.lookup().findStatic(Core.class,name,MethodType.methodType(operation==0 ? Carrier.class : Box.class,Context.class,Carrier.class,Class.class)).invokeWithArguments(context,seed,token);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable fault=mode==10||mode==12||mode==18 ? new AssertionError("owned failure") : mode==13||mode==19 ? new IOException("owned failure") : new IllegalStateException("owned failure");
        final IOException checked=mode==23 ? new FileNotFoundException("owned checked failure") : new IOException("owned checked failure");
        for(int attempt=0;attempt<2;++attempt) {
            final Context context=mode==3 ? null : new Context();
            final Carrier<?> seed=mode==1 ? null : mode==7 ? new Alien() : new Box();
            final Class<?> token=mode==2 ? null : String.class;
            final Carrier<?> alternate=mode==5 ? null : mode==6 ? new Alien() : new Box();
            final Carrier<?> replacement=new Box();
            final boolean useAlternate=mode==4||mode==5||mode==6||mode==20;
            Trace.events=0; Trace.context=null; Trace.seed=null; Trace.token=null; Trace.result=null;
            Trace.inputContext=context; Trace.inputSeed=seed; Trace.inputToken=token; Trace.useAlternate=mode==4||mode==5||mode==6;
            if(context!=null) {
                context.alternate=alternate;
                context.checked=mode==14||mode==16||mode==23 ? checked : null;
                context.action=mode==8 ? null : new Runnable() { public void run() {
                    check(Trace.context==context && Trace.seed==seed && Trace.token==token);
                    if(mode==11||mode==12||mode==13) ContextualCarrierTransferHarness.<RuntimeException>raise(fault);
                    if(mode==15) context.checked=checked;
                    if(mode==16) context.checked=null;
                    if(mode==20) { context.alternate=replacement; Trace.useAlternate=true; }
                    if(mode==22) { Trace.inputContext=new Context(); Trace.inputSeed=replacement; Trace.inputToken=Integer.class; }
                }};
            }
            Trace.initialize=new Runnable() { public void run() {
                if(mode==9||mode==10) ContextualCarrierTransferHarness.<RuntimeException>raise(fault);
                if(mode==21) { Trace.inputContext=new Context(); Trace.inputSeed=replacement; Trace.inputToken=Integer.class; }
            }};
            final Carrier<?> expected=useAlternate ? mode==20 ? replacement : alternate : seed;
            Trace.after=new Runnable() { public void run() {
                check(Trace.result==expected);
                if(mode==17||mode==18||mode==19) ContextualCarrierTransferHarness.<RuntimeException>raise(fault);
            }};
            Carrier<?> result=null; Throwable failure=null;
            try { result=invoke(route,operation,context,seed,token); } catch(Throwable e) { failure=e; }
            if(mode==9||mode==10) {
                check(result==null && Trace.context==null && Trace.seed==null && Trace.token==null && Trace.result==null);
                check(Trace.events==(attempt==0 ? 0x71L : 0x7L));
                if(attempt!=0) check(failure instanceof NoClassDefFoundError);
                else if(mode==9) check(failure instanceof ExceptionInInitializerError && failure.getCause()==fault);
                else check(failure==fault);
                continue;
            }
            check(Trace.context==context && Trace.seed==seed && Trace.token==token);
            long events=attempt==0 ? 0x7123L : 0x73L;
            boolean early=mode==3||mode==8||mode==11||mode==12||mode==13;
            boolean declared=mode==14||mode==15||mode==23;
            boolean cast=operation==1 && expected!=null && !(expected instanceof Box);
            boolean after=mode==17||mode==18||mode==19;
            if(!early) {
                events=events*16+4;
                if(!declared) { events=events*16+(useAlternate?5:6); if(!cast) { events=events*16+8; if(!after) events=events*16+9; } }
            }
            check(Trace.events==events);
            if(early) { check(result==null && Trace.result==null); if(mode==3||mode==8) check(failure instanceof NullPointerException); else check(failure==fault); }
            else if(declared) check(result==null && Trace.result==null && failure==checked);
            else if(cast) check(result==null && Trace.result==null && failure instanceof ClassCastException);
            else if(after) check(result==null && Trace.result==expected && failure==fault);
            else check(failure==null && result==expected && Trace.result==expected);
        }
        System.out.println("attempts=2,contextual-carrier-transfer=matched");
    }
}
