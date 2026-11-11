import fixture.*;
import java.io.IOException;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConditionalArrayLengthHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned conditional length contract");
    }
    static Target call(Holder holder, boolean flag, int route) throws Throwable {
        if (route == 0) return Core.make(holder, flag);
        if (route == 1) return ConditionalArrayLengthCaller.call(holder, flag);
        if (route == 2) {
            try { return (Target) Core.class.getMethod("make", Holder.class, boolean.class).invoke(null, holder, flag); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, "make",
            MethodType.methodType(Target.class, Holder.class, boolean.class)).invokeExact(holder, flag);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean flag = mode == 1 || mode == 3 || mode == 6 || mode == 14;
        int length = mode == 2 ? 0 : mode == 3 ? 2 : mode == 14 ? 1 : 5;
        Trace.prepared = mode == 4 ? null : new int[length];
        Trace.afterRead = new int[11];
        Trace.replacement = mode == 15 ? null : new int[13];
        Trace.holder = mode == 5 ? null : new Holder();
        Trace.value = 7;
        Trace.failAt = mode == 8 || mode == 9 ? 1 : mode == 10 ? 2
            : mode == 11 || mode == 13 ? 3 : mode == 12 ? 4 : 0;
        Trace.checked = mode == 10 || mode == 11 ? new IOException("owned checked failure") : null;
        Trace.error = mode == 9 || mode == 13 ? new AssertionError("owned error") : null;
        Trace.runtime = new IllegalStateException("owned runtime failure");
        Target previous = null;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = Trace.reads = Trace.constructors = 0;
            Trace.target = Trace.observed = null;
            if (Trace.holder != null) {
                Trace.holder.first = attempt == 0 ? new int[17] : Trace.prepared;
                Trace.holder.second = mode == 6 || mode == 7 ? null : new Object[3];
            }
            int expected = attempt == 0 ? 1 : 0;
            boolean classFailure = mode == 8 || mode == 9;
            boolean nullFailure = mode == 4 || mode == 5 || mode == 6;
            boolean read = !classFailure && !nullFailure && !flag;
            boolean constructor = !classFailure && !nullFailure && mode != 10;
            boolean assigned = constructor && mode != 11 && mode != 13;
            if (read) expected = expected * 10 + 2;
            if (constructor) expected = expected * 10 + 3;
            if (assigned) expected = expected * 10 + 4;
            Target result = null; Throwable failure = null;
            try { result = call(Trace.holder, flag, route); }
            catch (Throwable problem) { failure = problem; }
            require(Trace.events == expected && Trace.reads == (read ? 1 : 0)
                && Trace.constructors == (constructor ? 1 : 0));
            require(Trace.observed == (read ? Trace.holder : null));
            if (classFailure) {
                if (attempt == 1) require(failure instanceof NoClassDefFoundError);
                else if (mode == 9) require(failure == Trace.error);
                else require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.runtime);
            } else if (nullFailure) require(failure instanceof NullPointerException);
            else if (mode == 10 || mode == 11) require(failure == Trace.checked);
            else if (mode == 12) require(failure == Trace.runtime);
            else if (mode == 13) require(failure == Trace.error);
            else require(failure == null && result != null && result == Trace.target && result != previous);
            require((Trace.target != null) == constructor);
            if (constructor) {
                Target escaped = (Target) Trace.target;
                require(escaped.capacity == (assigned ? (length > 2 ? length - 2 : length) : 0));
                require(escaped.tail == (assigned ? (flag ? 3 : Trace.value) : 0));
            }
            if (Trace.holder != null && !classFailure)
                require(Trace.holder.first == (assigned ? Trace.replacement
                    : read && mode != 10 ? Trace.afterRead : Trace.prepared));
            if (result != null) previous = result;
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + Trace.events);
        }
    }
}
