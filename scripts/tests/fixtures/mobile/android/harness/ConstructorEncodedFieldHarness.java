import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ConstructorEncodedFieldHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("constructor encoded field contract");
    }
    static Base make(int route, Object value) throws Throwable {
        if (route==0) return Client.make(value);
        if (route==1) return ConstructorEncodedFieldCaller.make(value);
        if (route==2) {
            try { return Reader.class.getConstructor(Object.class).newInstance(value); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        if (route==3) {
            MethodHandle ctor=MethodHandles.lookup().findConstructor(Reader.class,MethodType.methodType(void.class,Object.class));
            return (Reader)ctor.invokeExact(value);
        }
        return new DirectReader(value);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        Trace.failure=new IllegalStateException("owned constructor field failure"); Trace.failAt=mode;
        Object value=mode==5 ? null : new Object();
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Trace.nextWord=55+attempt; Trace.nextWide=0x8123456789abcdefL+attempt;
            Base result=null; Throwable failure=null;
            try { result=make(route,value); } catch (Throwable error) { failure=error; }
            int events=mode==1 ? (attempt==0 ? 1 : 0) : (attempt==0 ? 1 : 0)*10+2;
            if (mode!=1 && mode!=2 && mode!=5) events=events*10+3;
            if (mode==0 || mode==4) events=events*10+4;
            check(Trace.events==events && Trace.baseCalls==(mode==1 ? 0 : attempt+1));
            if (mode==0) {
                check(failure==null && result.payload==value);
                check(result.word==(attempt==0 ? 37 : 55));
                check(result.wide==(attempt==0 ? 0xfedcba9876543210L : 0x8123456789abcdefL));
            } else if (mode==1) {
                if (attempt==0) check(failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
                else check(failure instanceof NoClassDefFoundError);
            } else check(failure==Trace.failure);
            for (String name : new String[]{"word","wide"}) {
                Field field=Holder.class.getField(name);
                check(Modifier.isStatic(field.getModifiers()) && !Modifier.isFinal(field.getModifiers()));
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
