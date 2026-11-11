import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationCallAliasHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("allocation call alias changed"); }
    static Target call(Holder holder, boolean flag, int route, int variant) throws Throwable {
        String name = variant == 0 ? "make" : "live";
        if (route == 0) return variant == 0 ? Core.make(holder, flag) : Core.live(holder, flag);
        if (route == 1) return variant == 0 ? AllocationCallAliasCaller.make(holder, flag) : AllocationCallAliasCaller.live(holder, flag);
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Holder.class, boolean.class).invoke(null, holder, flag); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(Target.class, Holder.class, boolean.class)).invokeExact(holder, flag);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.failAt = mode >= 1 && mode <= 4 ? mode : 0;
        Trace.failure = new RuntimeException("owned call alias failure");
        int[] input = {17}, replaced = {53}, output = {29}, first = {11};
        Trace.first = mode == 7 ? null : first;
        Trace.second = mode == 8 ? null : mode == 9 ? new Object() : input;
        Trace.result = mode == 6 ? null : output;
        Trace.replaced = replaced;
        Trace.holder = mode == 5 ? null : new Holder();
        Object sentinel = new Object(); Trace.after = sentinel;
        Target previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            if (Trace.holder != null) {
                Trace.holder.first = attempt == 0 ? sentinel : Trace.first;
                Trace.holder.second = attempt == 0 ? sentinel : Trace.second;
            }
            int oldCalls = Trace.calls, oldConstructors = Trace.constructors, oldInput = input[0];
            boolean flag = (mode & 1) != 0;
            Throwable failure = null; Target result = null;
            try { result = call(Trace.holder, flag, route, variant); } catch (Throwable error) { failure = error; }
            boolean enters = mode != 1 && mode != 2 && mode != 5;
            boolean constructs = enters && mode != 3 && mode != 9;
            int expected = mode == 1 ? (attempt == 0 ? 1 : 0)
                : mode == 2 ? (attempt == 0 ? 12 : 0)
                : mode == 5 ? (attempt == 0 ? 1 : 0)
                : (attempt == 0 ? 123 : 3) * (constructs ? 10 : 1) + (constructs ? 4 : 0);
            require(Trace.events == expected && Trace.calls == oldCalls + (enters ? 1 : 0));
            require(Trace.constructors == oldConstructors + (constructs ? 1 : 0));
            require(input[0] == oldInput + (enters && mode != 3 && mode != 8 && mode != 9 ? 1 : 0));
            if (enters) require(Trace.seen == Trace.second);
            if (mode == 1 || mode == 2) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 3 || mode == 4) require(failure == Trace.failure);
            else if (mode == 5) require(failure instanceof NullPointerException);
            else if (mode == 9) require(failure instanceof ClassCastException);
            else {
                require(failure == null && result != null && result != previous);
                require(result.first == Trace.first && result.second == Trace.result && result.flag == flag);
                require(Trace.after == Trace.result && Trace.holder.second == replaced);
                if (mode != 6) { output[0]++; require(((int[]) result.second)[0] == output[0]); }
                previous = result;
            }
            if (failure != null) require(Trace.after == sentinel);
            System.out.println("variant=" + variant + ",route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
