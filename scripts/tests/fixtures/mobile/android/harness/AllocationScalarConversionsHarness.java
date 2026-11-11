import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class AllocationScalarConversionsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("allocation scalar conversion contract");
    }
    static Target call(int entry, int route, Provider provider, Object input) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(provider, input);
            if (entry == 1) return Core.guarded(provider, input);
            return Core.locked(provider, input);
        }
        if (route == 1) return AllocationScalarConversionsCaller.call(entry, provider, input);
        String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Provider.class, Object.class).invoke(null, provider, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(Target.class, Provider.class, Object.class)).invoke(provider, input);
    }
    static void check(Target value, long number, double wide, float narrow,
                      byte small, short medium, char character, boolean flag) {
        require(value != null && value.number == number
                && Double.doubleToRawLongBits(value.wide) == Double.doubleToRawLongBits(wide)
                && Float.floatToRawIntBits(value.narrow) == Float.floatToRawIntBits(narrow)
                && value.small == small && value.medium == medium && value.character == character && value.flag == flag);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        boolean unary = args.length > 3 && Boolean.parseBoolean(args[3]);
        Trace.lock = new Object();
        Trace.expectLock = entry == 2;
        Trace.failure = new IllegalArgumentException("owned scalar failure");
        Trace.failAt = mode == 1 ? 1 : mode == 2 ? 8 : mode == 3 ? 9 : 0;
        Provider provider = new Provider();
        Trace.provider = provider;
        Trace.number = 0x123456789abcdef0L;
        Trace.wide = -123456789012345.75;
        Trace.narrow = -0.0f;
        Trace.integer = 0x89abcdef;
        Trace.flag = true;
        if (mode == 6) {
            Trace.wide = Double.longBitsToDouble(0x7ff8123456789abcL);
            Trace.narrow = Float.intBitsToFloat(0x7fc12345);
        } else if (mode == 7) {
            Trace.wide = Double.POSITIVE_INFINITY;
            Trace.narrow = Float.NEGATIVE_INFINITY;
        } else if (mode == 8) {
            Trace.number = Long.MAX_VALUE;
            Trace.wide = Double.NEGATIVE_INFINITY;
            Trace.integer = Integer.MIN_VALUE;
            Trace.narrow = Float.MIN_VALUE;
        } else if (mode == 9) {
            Trace.number = Long.MIN_VALUE;
            Trace.wide = -0.0;
            Trace.integer = Integer.MAX_VALUE;
            Trace.narrow = Float.MAX_VALUE;
        }
        int[] input = {17};
        Object argument = mode == 5 ? null : input;
        Provider receiver = mode == 4 ? null : provider;
        for (int attempt = 0; attempt < 2; attempt++) {
            provider.number = Long.MIN_VALUE;
            provider.wide = Double.NEGATIVE_INFINITY;
            provider.narrow = -0.0f;
            provider.integer = Integer.MIN_VALUE + 1;
            provider.flag = false;
            long number = (long) (attempt == 0 ? Trace.wide : provider.wide);
            double wide = (double) (attempt == 0 ? Trace.narrow : provider.narrow);
            float narrow = (float) (attempt == 0 ? Trace.number : provider.number);
            int integer = attempt == 0 ? Trace.integer : provider.integer;
            if (unary) {
                number = (long) (-(attempt == 0 ? Trace.wide : provider.wide));
                wide = (double) (-(attempt == 0 ? Trace.narrow : provider.narrow));
                narrow = (float) (~(-(attempt == 0 ? Trace.number : provider.number)));
                integer = ~(-integer);
            }
            byte small = (byte) integer;
            short medium = (short) integer;
            char character = (char) integer;
            boolean flag = attempt == 0 ? Trace.flag : provider.flag;
            Trace.events = 0; Trace.last = null; Trace.published = null; Trace.seen = null;
            int oldInput = input[0];
            Target result = null;
            Throwable thrown = null;
            try { result = call(entry, route, receiver, argument); } catch (Throwable failure) { thrown = failure; }
            require(!Thread.holdsLock(Trace.lock));
            long expected = attempt == 0 ? 9 : 0;
            boolean constructor = mode != 1 && mode != 3 && mode != 4 && mode != 5;
            int completed = mode != 1 && mode != 3 && mode != 4 && mode != 5 ? 1 : 0;
            if (mode != 3 && mode != 4) expected = expected * 10 + 1;
            if (constructor) expected = expected * 10 + 8;
            require(Trace.events == expected && input[0] == oldInput + completed);
            require(Trace.seen == (mode != 1 && mode != 3 && mode != 4 ? argument : null));
            boolean failed = mode >= 1 && mode <= 5;
            Throwable observed = entry == 1 ? Trace.last : thrown;
            require(entry != 1 || thrown == null);
            if (!failed) require(observed == null && result == Trace.published);
            else {
                require(entry != 1 || result == null);
                if (mode == 3) {
                    if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                    else require(observed instanceof NoClassDefFoundError);
                } else if (mode == 4 || mode == 5) require(observed instanceof NullPointerException);
                else require(observed == Trace.failure);
            }
            require(constructor ? Trace.published != null : Trace.published == null);
            if (constructor) check(Trace.published, number, wide, narrow, small, medium, character, flag);
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
