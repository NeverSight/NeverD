import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocatedScopedCallbackHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned allocated scoped callback");
    }
    static Child<?> make(int route, Object marker) throws Throwable {
        if (route==0) return new Child<Object>(marker);
        if (route==1) return AllocatedScopedCallbackCaller.make(marker);
        if (route==2) try { return (Child<?>)Child.class.getConstructor(Object.class).newInstance(marker); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
        return (Child<?>)MethodHandles.publicLookup().findConstructor(Child.class,
                MethodType.methodType(void.class,Object.class)).invokeExact(marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]);
        Trace.fail=mode>=1&&mode<=7 ? mode : 0;
        Trace.failure=new IllegalArgumentException("owned callback construction failure");
        Object seed=new Object(), late=new Object(), marker=mode==8 ? null : new Object();
        Trace.late=late;
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0; Trace.parent=null; Trace.child=null; Trace.adapter=null; Trace.seed=seed;
            Throwable failure=null; Child<?> result=null;
            try { result=make(route,marker); } catch (Throwable actual) { failure=actual; }
            boolean initializationFailure=mode>=1&&mode<=3;
            int end=attempt==1&&initializationFailure ? 0 : mode>=1&&mode<=7 ? mode : 7;
            long expected=0;
            for (int event=attempt==0 ? 1 : 4;event<=end;event++) expected=expected*17+event;
            require(Trace.events==expected);
            require((Trace.adapter!=null)==(end>=4));
            if (end>=4) require(Trace.adapter.snapshot==seed);
            require((Trace.parent!=null)==(end>=5));
            if (end>=5) {
                Base<?> parent=(Base<?>)Trace.parent;
                require(parent.factory==Trace.adapter && parent.value==(end>=7 ? seed : null));
                require(((Child<?>)parent).marker==(end>=7 ? marker : null));
                require(Trace.seed==late);
            } else require(Trace.seed==seed);
            require(Trace.child==(end>=7 ? Trace.parent : null));
            if (initializationFailure) {
                if (attempt==0) require(failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode>=4&&mode<=7) require(failure==Trace.failure);
            else require(failure==null && result==Trace.parent && result.marker==marker);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
