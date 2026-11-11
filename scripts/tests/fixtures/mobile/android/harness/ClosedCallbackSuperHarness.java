import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedCallbackSuperHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed callback super mismatch");
    }
    static Object field(Object value, String name) throws Exception {
        Field field=value.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(value);
    }
    static Core.Child make(int route, Context context, CharSequence text) throws Throwable {
        if (route==0) return Core.make(context,text);
        if (route==1) return ClosedCallbackSuperCaller.make(context,text);
        if (route==2) try { return Core.Child.class.getConstructor(Context.class,CharSequence.class).newInstance(context,text); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
        return (Core.Child)MethodHandles.publicLookup().findConstructor(Core.Child.class,
            MethodType.methodType(void.class,Context.class,CharSequence.class)).invokeExact(context,text);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Trace.fail=mode>=1&&mode<=7 ? mode : 0;
        Trace.failure=new IllegalArgumentException("owned callback failure");
        String seed=mode==9 ? null : new String("owned snapshot");
        String late=new String("owned late field");
        CharSequence text=mode==8 ? null : new StringBuilder("owned text input");
        Context context=new Context();
        Trace.context=context;Trace.late=late;
        for (int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.adapter=null;Trace.parent=null;Trace.child=null;context.reply=seed;
            Throwable failure=null;Core.Child result=null;
            try { result=make(route,context,text); } catch (Throwable actual) { failure=actual; }
            boolean initializationFailure=mode>=1&&mode<=3;
            int end=attempt==1&&initializationFailure ? 0 : mode>=1&&mode<=7 ? mode : 7;
            long expected=0;
            for (int event=attempt==0 ? 1 : 4;event<=end;event++) expected=expected*17+event;
            require(Trace.events==expected);
            require((Trace.adapter!=null)==(end>=4));
            if (end>=4) require(field(Trace.adapter,"context")==context
                && field(Trace.adapter,"text")==text && field(Trace.adapter,"snapshot")==seed);
            require((Trace.parent!=null)==(end>=5));
            if (end>=5) {
                Base<?> parent=(Base<?>)Trace.parent;
                require(parent.factory==Trace.adapter && parent.value==(end>=7 ? seed : null));
                require(((Core.Child)parent).marker==(end>=7 ? text : null));
                require(context.reply==late);
            } else require(context.reply==seed);
            require(Trace.child==(end>=7 ? Trace.parent : null));
            if (initializationFailure) {
                if (attempt==0) require(failure instanceof ExceptionInInitializerError && failure.getCause()==Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode>=4&&mode<=7) require(failure==Trace.failure);
            else require(failure==null && result==Trace.parent && result.marker==text);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
