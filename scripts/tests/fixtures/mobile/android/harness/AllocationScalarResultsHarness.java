import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class AllocationScalarResultsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("allocation scalar result contract");
    }
    static Target call(int entry, int route, Provider provider, Object input) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(provider, input);
            if (entry == 1) return Core.guarded(provider, input);
            return Core.locked(provider, input);
        }
        if (route == 1) return AllocationScalarResultsCaller.call(entry, provider, input);
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
        Trace.lock = new Object();
        Trace.expectLock = entry == 2;
        Trace.failure = new IllegalArgumentException("owned scalar failure");
        Trace.failAt = mode <= 9 ? mode : 0;
        Provider provider = new Provider();
        Trace.provider = provider;
        Trace.number = 0x123456789abcdef0L;
        Trace.wide = Double.longBitsToDouble(0x7ff8123456789abcL);
        Trace.narrow = Float.intBitsToFloat(0x7fc12345);
        Trace.small = Byte.MIN_VALUE;
        Trace.medium = Short.MIN_VALUE;
        Trace.character = '\ud800';
        Trace.flag = true;
        int[] input = {17};
        Object argument = mode == 11 ? null : input;
        Provider receiver = mode == 10 ? null : provider;
        for (int attempt = 0; attempt < 2; attempt++) {
            provider.number = Long.MIN_VALUE;
            provider.wide = -0.0;
            provider.narrow = -0.0f;
            provider.small = Byte.MAX_VALUE;
            provider.medium = Short.MAX_VALUE;
            provider.character = '\uffff';
            provider.flag = false;
            long number = attempt == 0 ? Trace.number : provider.number;
            double wide = attempt == 0 ? Trace.wide : provider.wide;
            float narrow = attempt == 0 ? Trace.narrow : provider.narrow;
            byte small = attempt == 0 ? Trace.small : provider.small;
            short medium = attempt == 0 ? Trace.medium : provider.medium;
            char character = attempt == 0 ? Trace.character : provider.character;
            boolean flag = attempt == 0 ? Trace.flag : provider.flag;
            Trace.events = 0; Trace.last = null; Trace.published = null; Trace.seen = null;
            int oldInput = input[0];
            Target result = null;
            Throwable thrown = null;
            try { result = call(entry, route, receiver, argument); } catch (Throwable failure) { thrown = failure; }
            require(!Thread.holdsLock(Trace.lock));
            long expected = attempt == 0 ? 9 : 0;
            int completed = 0, entered = 0;
            boolean constructor = false;
            if (mode != 9 && mode != 10) {
                for (int event = 1; event <= 7; event++) {
                    expected = expected * 10 + event; entered++;
                    if (mode == event || mode == 11) break;
                    completed++;
                }
                if (completed == 7) {
                    expected = expected * 10 + 8;
                    constructor = true;
                }
            }
            require(Trace.events == expected && input[0] == oldInput + completed);
            require(Trace.seen == (completed > 0 ? argument : null));
            boolean failed = mode != 0;
            Throwable observed = entry == 1 ? Trace.last : thrown;
            require(entry != 1 || thrown == null);
            if (!failed) require(observed == null && result == Trace.published);
            else {
                require(entry != 1 || result == null);
                if (mode == 9) {
                    if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                    else require(observed instanceof NoClassDefFoundError);
                } else if (mode >= 10) require(observed instanceof NullPointerException);
                else require(observed == Trace.failure);
            }
            require(constructor ? Trace.published != null : Trace.published == null);
            if (constructor) check(Trace.published, number, wide, narrow, small, medium, character, flag);
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
