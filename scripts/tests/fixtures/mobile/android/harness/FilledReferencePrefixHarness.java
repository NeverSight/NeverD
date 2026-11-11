package fixture;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.Arrays;
public final class FilledReferencePrefixHarness {
    static void check(boolean value) {
        if(!value) throw new AssertionError("owned filled reference prefix mismatch");
    }
    @SuppressWarnings("unchecked") static <E extends Throwable> void raise(Throwable e) throws E { throw (E)e; }
    static Core call(int route,int operation,Object a,Object b) throws Throwable {
        if(route==0) {
            switch(operation) {
            case 0: return new Core(a,b);
            case 1: return new Core((Object[])a,(Object[])b);
            case 2: return new Core(a,b,1);
            default: return new Core((Object[])a,(Object[])b,1);
            }
        }
        if(route==1) return FilledReferencePrefixCaller.create(operation,a,b);
        Class<?> type=(operation&1)==0 ? Object.class : Object[].class;
        Class<?>[] types=operation<2 ? new Class<?>[]{type,type} : new Class<?>[]{type,type,int.class};
        Object[] arguments=operation<2 ? new Object[]{a,b} : new Object[]{a,b,1};
        if(route==2) {
            try { return Core.class.getConstructor(types).newInstance(arguments); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        return (Core)MethodHandles.lookup().findConstructor(Core.class,MethodType.methodType(void.class,types)).invokeWithArguments(Arrays.asList(arguments));
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final boolean matrix=(operation&1)!=0;
        final Throwable fault=mode==6||mode==8||mode==12||mode==15 ? new AssertionError("owned failure") :
            mode==9||mode==11||mode==13 ? new IOException("owned failure") : new IllegalStateException("owned failure");
        check(Modifier.isFinal(Base.class.getField("values").getModifiers()));
        Object[] previous=null;
        for(int attempt=0;attempt<2;++attempt) {
            final Object a=mode==1 ? null : matrix ? new Object[]{new Object()} : new Object();
            final Object b=mode==1 ? null : mode==2 ? a : matrix ? new Object[]{new Object()} : new Object();
            final Object[] alternate=new Object[]{new Object()};
            Trace.events=0; Trace.first=a; Trace.second=b; Trace.supplied=null;
            Trace.replacement=alternate; Trace.substitute=mode==4; Trace.baseSelf=null; Trace.coreSelf=null;
            Trace.initialize=new Runnable() { public void run() {
                Trace.first=new Object(); Trace.second=new Object();
                if(mode==5||mode==6) FilledReferencePrefixHarness.<RuntimeException>raise(fault);
            }};
            Trace.prepare=new Runnable() { public void run() {
                check(Trace.supplied!=null && Trace.supplied.length==2);
                check(Trace.supplied.getClass()==(matrix ? Object[][].class : Object[].class));
                check(Trace.supplied[0]==a && Trace.supplied[1]==b);
                if(mode==3) Trace.supplied[0]=b;
                if(mode==7||mode==8||mode==9) FilledReferencePrefixHarness.<RuntimeException>raise(fault);
            }};
            Trace.baseAction=new Runnable() { public void run() {
                check(Trace.baseSelf.values==(mode==4 ? alternate : Trace.supplied));
                if(mode==10||mode==12||mode==13) FilledReferencePrefixHarness.<RuntimeException>raise(fault);
            }};
            Trace.after=new Runnable() { public void run() {
                check(Trace.coreSelf==Trace.baseSelf);
                if(mode==11||mode==14||mode==15) FilledReferencePrefixHarness.<RuntimeException>raise(fault);
            }};
            Core result=null; Throwable failure=null;
            try { result=call(route,operation,a,b); } catch(Throwable e) { failure=e; }
            boolean initFailure=mode==5||mode==6;
            boolean factoryFailure=mode==7||mode==8||mode==9;
            boolean baseFailure=mode==10||mode==12||mode==13;
            boolean tailFailure=mode==11||mode==14||mode==15;
            if(initFailure) {
                check(result==null && Trace.supplied==null && Trace.baseSelf==null && Trace.coreSelf==null);
                check(Trace.events==(attempt==0 ? 0x1L : 0L));
                if(attempt!=0) check(failure instanceof NoClassDefFoundError);
                else if(mode==5) check(failure instanceof ExceptionInInitializerError && failure.getCause()==fault);
                else check(failure==fault);
                continue;
            }
            check(Trace.supplied!=null && Trace.supplied.length==2 && Trace.supplied!=previous);
            previous=Trace.supplied;
            check(Trace.supplied[0]==(mode==3 ? b : a) && Trace.supplied[1]==b);
            long stages=factoryFailure ? 0x3L : baseFailure ? 0x345L : tailFailure ? 0x34567L : 0x345678L;
            int length=factoryFailure ? 1 : baseFailure ? 3 : tailFailure ? 5 : 6;
            check(Trace.events==(attempt==0 ? (0x12L<<(length*4))|stages : stages));
            if(factoryFailure||baseFailure||tailFailure) check(result==null && failure==fault);
            else check(failure==null && result==Trace.coreSelf && result==Trace.baseSelf);
            check((Trace.baseSelf==null)==factoryFailure);
            check((Trace.coreSelf==null)==(factoryFailure||baseFailure));
            if(Trace.baseSelf!=null) check(Trace.baseSelf.values==(mode==4 ? alternate : Trace.supplied));
        }
        System.out.println("attempts=2,filled-reference-prefix=matched");
    }
}
