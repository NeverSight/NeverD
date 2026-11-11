import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedInterfaceSuperHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("owned closed interface super");
    }
    static Result call(int route, Host.Child receiver, Item item) throws Throwable {
        if (route == 0) return receiver.choose(item);
        if (route == 1) return ClosedInterfaceSuperCaller.call(receiver, item);
        if (route == 2) return ((Contract.Api)receiver).choose(item);
        if (route == 3) {
            try { return (Result)Host.Child.class.getMethod("choose", Item.class).invoke(receiver, item); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findVirtual(Host.Child.class, "choose", MethodType.methodType(Result.class, Item.class));
        return (Result)handle.invokeExact(receiver, item);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        check(Host.Child.class.getEnclosingClass() == Host.class && Modifier.isStatic(Host.Child.class.getModifiers()));
        check(Contract.Api.class.getEnclosingClass() == Contract.class && Contract.Api.class.getTypeParameters().length == 0);
        Method method = Contract.Api.class.getMethod("choose", Item.class);
        check(method.isDefault() && method.getGenericReturnType() == Result.class && method.getTypeParameters().length == 0);
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.failAt = Trace.swapAt = 0;
            Result first = new Result(new Object(), new Special()), late = new Result(new Object(), new Special());
            boolean branch = mode == 1 || (mode >= 6 && mode <= 8) || mode == 10 || mode == 13 || mode == 14;
            Item item = mode == 2 ? null : branch ? new Special() : new Item();
            Host.Child receiver = mode == 3 ? null : new Host.Child();
            Trace.events = 0; Trace.receiver = null; Trace.item = null; Trace.constructed = null;
            Trace.fallback = first; Trace.late = late;
            Trace.failAt = mode == 4 ? 1 : mode == 5 ? 2 : mode == 6 ? 3 : mode == 7 ? 4 : mode == 8 ? 5 : mode == 9 ? -1 : mode == 10 ? -4 : mode == 14 ? -5 : 0;
            Trace.swapAt = mode == 11 ? 1 : mode == 12 ? 2 : mode == 13 ? 4 : 0;
            Trace.failure = new IllegalArgumentException("owned failure"); Trace.error = new LinkageError("owned error");
            Result actual = null; Throwable caught = null;
            try { actual = call(route, receiver, item); } catch (Throwable failure) { caught = failure; }
            int expected = receiver == null ? 0 : branch ? mode == 6 ? 3 : mode == 7 || mode == 10 ? 34 : 345 : mode == 4 || mode == 9 ? 1 : 12;
            if (receiver == null) check(caught instanceof NullPointerException);
            else if (Trace.failAt < 0) check(caught == Trace.error);
            else if (Trace.failAt > 0) check(caught == Trace.failure);
            else check(caught == null && actual == (branch ? Trace.constructed : mode == 11 ? late : first));
            check(Trace.events == expected);
            boolean assigned = receiver != null && !branch && mode != 4 && mode != 9;
            check(Trace.receiver == (assigned ? receiver : null) && Trace.item == (assigned ? item : null));
            boolean constructed = receiver != null && branch && mode != 6 && mode != 7 && mode != 10;
            check((Trace.constructed != null) == constructed);
            if (constructed) check(Trace.constructed.receiver == receiver && Trace.constructed.item == item);
            if (caught != null) check(actual == null);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + expected);
        }
    }
}
