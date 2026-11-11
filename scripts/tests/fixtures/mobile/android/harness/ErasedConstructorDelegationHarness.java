import fixture.*;
import kotlin.jvm.functions.Function1;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ErasedConstructorDelegationHarness {
    static int mode;
    static RuntimeException original;
    static Function1 callback;
    static int calls;
    static long bits;
    static void require(boolean value) { if(!value) throw new AssertionError("erased constructor delegation changed"); }
    static Core make(int route, DefaultConstructorMarker marker) throws Throwable {
        if(route == 0) return ErasedConstructorDelegationCaller.make(7, -9, bits, callback);
        if(route == 1 || route == 3) {
            Constructor<Core> ctor = route == 1 ? Core.class.getConstructor(int.class, int.class, long.class, Function1.class, DefaultConstructorMarker.class)
                    : Core.class.getDeclaredConstructor(int.class, int.class, long.class, Function1.class);
            if(route == 3) ctor.setAccessible(true);
            try { return route == 1 ? ctor.newInstance(7, -9, bits, callback, marker) : ctor.newInstance(7, -9, bits, callback); }
            catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        if(route == 2) return (Core) MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class, int.class, int.class, long.class, Function1.class, DefaultConstructorMarker.class))
                .invokeExact(7, -9, bits, callback, marker);
        try { return (Core) Core.Factory.class.getMethod("make", int.class, int.class, long.class, Function1.class)
                    .invoke(new Core.Factory(), 7, -9, bits, callback); }
        catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        boolean synthetic = Boolean.parseBoolean(args[2]);
        require(Core.class.getConstructor(int.class,int.class,long.class,Function1.class,DefaultConstructorMarker.class).isSynthetic() == synthetic);
        require(Modifier.isPrivate(Core.class.getDeclaredConstructor(int.class,int.class,long.class,Function1.class).getModifiers()));
        original = new RuntimeException("owned constructor failure"); Trace.failure = original; Trace.failBody = mode == 3;
        bits = (mode & 1) == 0 ? 0x7ff8000000000031L : 0x8000000000000007L;
        callback = mode == 1 ? null : (Function1<Object,Object>) value -> {
            calls++; Trace.step(6);
            require(value == Trace.seen && value instanceof Core);
            Core object = (Core) value;
            require(object.getFirst() == 7 && object.getSecond() == -9 && object.getBits() == bits
                    && object.getCallback() == callback && !object.getAnswer());
            if(mode == 2) throw original;
            return mode == 4 ? null : mode == 5 ? "wrong carrier" : mode != 6;
        };
        DefaultConstructorMarker marker = null;
        if(mode == 7) {
            Constructor<DefaultConstructorMarker> ctor = DefaultConstructorMarker.class.getDeclaredConstructor();
            ctor.setAccessible(true); marker = ctor.newInstance();
        }
        for(int attempt = 0; attempt < 2; attempt++) {
            Trace.value = 0; Trace.seen = null; int oldCalls = calls;
            Core result = null; Throwable failure = null;
            try { result = make(route, marker); } catch(Throwable error) { failure = error; }
            boolean guardedNull = mode == 1 && (route == 0 || route == 4);
            int trace = guardedNull ? 0 : mode == 1 ? 2 : mode == 2 || mode == 4 || mode == 5 ? 26 : 263;
            require(Trace.value == trace && calls == oldCalls + (mode == 1 ? 0 : 1));
            if(guardedNull) require(Trace.seen == null); else {
                require(Trace.seen instanceof Core); Core seen = (Core) Trace.seen;
                require(seen.getFirst() == 7 && seen.getSecond() == -9 && seen.getBits() == bits && seen.getCallback() == callback);
                require(seen.getAnswer() == (mode == 0 || mode == 3 || mode == 7));
            }
            if(mode == 0 || mode == 6 || mode == 7) require(failure == null && result == Trace.seen);
            else {
                require(result == null);
                if(mode == 2 || mode == 3) require(failure == original);
                if(mode == 1 || mode == 4) require(failure instanceof NullPointerException);
                if(mode == 5) require(failure instanceof ClassCastException);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.value);
        }
    }
}
