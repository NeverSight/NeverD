import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationIntegerArithmeticHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("integer arithmetic allocation contract"); }
    static Target make(int kind, int route, int x, int y, int delta) throws Throwable {
        try {
            if (route == 0) return kind == 0 ? Core.make(x, y, delta) : Core.bias(x, y);
            if (route == 1) return AllocationIntegerArithmeticCaller.make(kind, x, y, delta);
            if (route == 2) return kind == 0
                ? (Target) Core.class.getMethod("make", int.class, int.class, int.class).invoke(null, x, y, delta)
                : (Target) Core.class.getMethod("bias", int.class, int.class).invoke(null, x, y);
            if (kind == 0) return (Target) MethodHandles.publicLookup().findStatic(Core.class, "make",
                MethodType.methodType(Target.class, int.class, int.class, int.class)).invokeExact(x, y, delta);
            return (Target) MethodHandles.publicLookup().findStatic(Core.class, "bias",
                MethodType.methodType(Target.class, int.class, int.class)).invokeExact(x, y);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]), route = Integer.parseInt(args[2]);
        int[][] inputs = {{Integer.MIN_VALUE,Integer.MAX_VALUE,1}, {Integer.MAX_VALUE,Integer.MIN_VALUE,-1},
            {Integer.MIN_VALUE,Integer.MIN_VALUE,Integer.MIN_VALUE}, {0,0,0}, {17,-23,3},
            {Integer.MAX_VALUE,Integer.MAX_VALUE,Integer.MAX_VALUE}};
        int[] input = inputs[variant < 6 ? variant : 4];
        int x = input[0], y = input[1], delta = kind == 0 ? input[2] : 13;
        Trace.failAt = variant == 6 ? 1 : variant == 7 ? 7 : 0;
        Trace.failure = new IllegalStateException("owned integer allocation failure");
        Target previous = null; long expectedTrace = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Trace.x = x; Trace.y = y; Trace.delta = input[2];
            Throwable caught = null;
            try {
                Target result = make(kind, route, Trace.x, Trace.y, Trace.delta);
                require(Trace.failAt == 0 && result != previous && result.left == x - delta
                    && result.top == y - delta && result.right == x + delta && result.bottom == y + delta);
                previous = result;
            } catch (Throwable failure) { caught = failure; }
            if (Trace.failAt == 1) {
                if (attempt == 1) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else if (Trace.failAt == 7) require(caught == Trace.failure);
            else require(caught == null);
            if (attempt == 1) expectedTrace = Trace.failAt == 1 ? 1 : 17;
            else if (Trace.failAt != 1) expectedTrace = expectedTrace * 10 + 7;
            require(Trace.events == expectedTrace);
        }
        System.out.println("variant=" + variant + ",kind=" + kind + ",route=" + route + ",trace=" + Trace.events);
    }
}
