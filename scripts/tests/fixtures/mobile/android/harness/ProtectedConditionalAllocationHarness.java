import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedConditionalAllocationHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned protected conditional allocation"); }
    static Target call(int route,Maker maker,Provider provider,Object seed) throws Throwable {
        try {
            if (route==0) return maker.make(provider,seed);
            if (route==1) return ProtectedConditionalAllocationCaller.make(maker,provider,seed);
            if (route==2) return (Target)Maker.class.getMethod("make",Provider.class,Object.class).invoke(maker,provider,seed);
            return (Target)MethodHandles.publicLookup().findVirtual(Maker.class,"make",
                    MethodType.methodType(Target.class,Provider.class,Object.class)).invokeExact(maker,provider,seed);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Trace.failAt=mode; Trace.failure=new IllegalArgumentException("owned failure");
        Maker maker=new Maker();Provider provider=new Provider();
        Object seed=new Object(),data=new int[]{19},sentinel=new Object();
        provider.first=variant==1 ? null : variant==2 ? new Object() : new Checked(seed);
        provider.second=variant==3 ? null : variant==4 ? new Object() : new Checked(variant==5 ? null : data);
        long expected=0;Target previous=null;
        for (int attempt=0;attempt<2;attempt++) {
            provider.calls=0;provider.data=attempt==0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Trace.observed=sentinel;
            int[] sequence=variant==6 ? new int[]{1} : variant==1 || variant==2
                    ? new int[]{1,2,5,6} : variant==3 || variant==4 ? new int[]{1,2,3}
                    : new int[]{1,2,3,4,5,6};
            boolean failed=variant==3 || variant==4 || variant==6;
            boolean assigned=false;int calls=0;
            for (int point:sequence) {
                if (point==1 && attempt==1) continue;
                expected=expected*10+point;
                if (point==2 || point==3) calls++;
                if (point==5) assigned=true;
                if (point==mode) { failed=true;break; }
            }
            boolean initFailed=mode==1;
            if (initFailed && attempt==1) { expected=Trace.events;calls=0; }
            if (failed && !initFailed) expected=expected*10+7;
            expected=expected*10+8;
            Throwable caught=null;Target result=null;
            try { result=call(route,maker,variant==6 ? null : provider,seed); }
            catch (Throwable failure) { caught=failure; }
            if (mode==8 || (mode==7 && failed && !initFailed)) require(caught==Trace.failure);
            else if (initFailed) {
                if (attempt==0) require(caught instanceof ExceptionInInitializerError && caught.getCause()==Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (mode>=2 && mode<=6 && failed) require(caught==Trace.failure);
            else if (variant==3 || variant==6) require(caught instanceof NullPointerException);
            else if (variant==4) require(caught instanceof ClassCastException);
            else require(caught==null && result!=previous && result.value==(variant==1 || variant==2 || variant==5 ? null : data)
                    && result.bits==provider.data);
            if (failed && !initFailed) require(Trace.observed==(assigned ? variant==1 || variant==2 || variant==5 ? null : data : seed));
            else require(Trace.observed==sentinel);
            require(Trace.events==expected && provider.calls==calls);
            previous=result;
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
