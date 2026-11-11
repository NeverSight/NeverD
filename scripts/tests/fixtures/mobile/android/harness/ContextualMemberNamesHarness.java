import fixture.yield.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ContextualMemberNamesHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("contextual member name contract"); }
    static int call(int route, Core target, int value) throws Throwable {
        try {
            if (route == 0) return Core.call(target, value);
            if (route == 1) return ContextualMemberNamesCaller.call(target, value);
            if (route == 2) return (Integer) Core.class.getMethod("call", Core.class, int.class).invoke(null, target, value);
            return (int) MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(int.class, Core.class, int.class)).invokeExact(target, value);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int[] inputs = {Integer.MIN_VALUE, Integer.MAX_VALUE, 0, 17, -23, 987654321};
        int value = inputs[variant < 6 ? variant : 3];
        Core target = variant == 6 ? null : new Core();
        RuntimeException failure = new IllegalStateException("owned contextual member failure");
        if (target != null) {
            target.yield = Integer.MIN_VALUE; target.record = 31;
            target.sealed = 41; target.permits = 59; target.var = -67;
            if (variant == 7) target.failure = failure;
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            Throwable caught = null;
            try {
                int result = call(route, target, value);
                require(variant < 6 && result == (((value + Integer.MIN_VALUE) ^ 31)
                    + (41 - value) + (59 ^ value) + value * 17 + (value - 23)));
                require(ContextualMemberNamesCaller.inherited(target, value) == ((value + Integer.MIN_VALUE) ^ 31));
                for (String name : new String[] {"yield", "record", "sealed", "permits", "var"})
                    require(Base.class.getField(name).getType() == int.class);
                require(Core.class.getMethod("yield", int.class).getReturnType() == int.class);
                require(Core.class.getMethod("sealed", int.class).getReturnType() == int.class);
                require(Core.class.getMethod("permits", int.class).getReturnType() == int.class);
                require(Core.class.getMethod("var", int.class).getReturnType() == int.class);
                require(Base.class.getMethod("record", int.class).getReturnType() == int.class);
            } catch (Throwable actual) { caught = actual; }
            require(variant == 6 ? caught instanceof NullPointerException
                : variant == 7 ? caught == failure : caught == null);
        }
        System.out.println("variant=" + variant + ",route=" + route + ",ok");
    }
}
