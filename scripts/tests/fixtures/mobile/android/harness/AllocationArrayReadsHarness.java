import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class AllocationArrayReadsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned allocation array read contract");
    }
    static Target call(int entry, int route, Provider p, int index, Object input) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.create(p, index, input);
            if (entry == 1) return Core.guarded(p, index, input);
            return Core.locked(p, index, input);
        }
        if (route == 1) return AllocationArrayReadsCaller.call(entry, p, index, input);
        String name = entry == 0 ? "create" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return (Target) Core.class.getMethod(name, Provider.class, int.class, Object.class).invoke(null, p, index, input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(Target.class, Provider.class, int.class, Object.class)).invoke(p, index, input);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.lock = new Object(); Trace.expectLock = entry == 2;
        Trace.fail = mode == 1 ? 9 : mode == 2 ? 1 : mode == 3 ? 8 : 0;
        Trace.failure = new IllegalArgumentException("owned array read failure");
        Provider p = new Provider(); Trace.provider = p;
        Trace.reference = new Object(); Trace.wide = -0.0; Trace.narrow = -0.0f;
        Trace.number = 0x123456789abcdef0L; Trace.integer = Integer.MIN_VALUE;
        Trace.flag = true; Trace.small = -128; Trace.character = (char) 65535; Trace.medium = -32768;
        if (mode == 8) {
            Trace.wide = Double.longBitsToDouble(0x7ff8123456789abcL);
            Trace.narrow = Float.intBitsToFloat(0x7fc12345);
        } else if (mode == 9) { Trace.wide = Double.POSITIVE_INFINITY; Trace.narrow = Float.MIN_VALUE; }
        if (mode == 5) p.reference = null;
        Provider receiver = mode == 4 ? null : p;
        int index = mode == 6 ? -1 : mode == 7 ? 1 : 0;
        int[] input = {17}; Trace.input = input;
        for (int attempt = 0; attempt < 2; attempt++) {
            Object reference = attempt == 0 ? Trace.reference : p.reference == null ? null : p.reference[0];
            double wide = attempt == 0 ? Trace.wide : p.wide[0];
            float narrow = attempt == 0 ? Trace.narrow : p.narrow[0];
            long number = attempt == 0 ? Trace.number : p.number[0];
            int integer = attempt == 0 ? Trace.integer : p.integer[0];
            boolean flag = attempt == 0 ? Trace.flag : p.flag[0];
            byte small = attempt == 0 ? Trace.small : p.small[0];
            char character = attempt == 0 ? Trace.character : p.character[0];
            short medium = attempt == 0 ? Trace.medium : p.medium[0];
            Trace.events = 0; Trace.last = null; Trace.seen = null; Trace.published = null;
            int old = input[0]; Target result = null; Throwable thrown = null;
            try { result = call(entry, route, receiver, index, input); } catch (Throwable failure) { thrown = failure; }
            long events = attempt == 0 ? 9 : 0;
            boolean callback = mode != 1 && mode < 4 || mode >= 8;
            if (callback) events = events * 11 + 1;
            boolean constructor = callback && mode != 2;
            if (constructor) events = events * 11 + 8;
            require(Trace.events == events && !Thread.holdsLock(Trace.lock));
            require(input[0] == old + (constructor ? 1 : 0) && Trace.seen == (callback ? input : null));
            Throwable observed = entry == 1 ? Trace.last : thrown;
            require(entry != 1 || thrown == null);
            if (mode == 0 || mode >= 8) require(observed == null && result == Trace.published);
            else if (mode == 1) {
                if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                else require(observed instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3) require(observed == Trace.failure);
            else if (mode == 4 || mode == 5) require(observed instanceof NullPointerException);
            else require(observed instanceof ArrayIndexOutOfBoundsException);
            require(constructor ? Trace.published != null : Trace.published == null);
            if (constructor) {
                Target value = Trace.published;
                require(value.reference == reference && Double.doubleToRawLongBits(value.wide) == Double.doubleToRawLongBits(wide)
                        && Float.floatToRawIntBits(value.narrow) == Float.floatToRawIntBits(narrow) && value.number == number
                        && value.integer == integer && value.flag == flag && value.small == small
                        && value.character == character && value.medium == medium && value.after == 17);
            }
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
