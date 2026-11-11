import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationFieldAliasHarness {
    static int input, floatBits;
    static long bits, doubleBits;
    static void require(boolean value) { if(!value) throw new AssertionError("allocation field alias changed"); }
    static Target make(int route) throws Throwable {
        float floating = Float.intBitsToFloat(floatBits); double fraction = Double.longBitsToDouble(doubleBits);
        if(route == 0) return Core.make(input, bits, floating, fraction);
        if(route == 1) return AllocationFieldAliasCaller.make(input, bits, floating, fraction);
        if(route == 2) {
            try { return (Target) Core.class.getMethod("make", int.class,long.class,float.class,double.class)
                    .invoke(null,input,bits,floating,fraction); }
            catch(InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target) MethodHandles.publicLookup().findStatic(Core.class,"make",
                MethodType.methodType(Target.class,int.class,long.class,float.class,double.class))
                .invokeExact(input,bits,floating,fraction);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        input = mode == 6 ? Integer.MAX_VALUE : mode == 7 ? Integer.MIN_VALUE : -19;
        bits = (mode & 1) == 0 ? 0x7ff8000000000031L : 0x8000000000000007L;
        floatBits = (mode & 1) == 0 ? 0x7fc01234 : 0x80000000;
        doubleBits = (mode & 1) == 0 ? 0x7ff8000000000057L : 0x8000000000000000L;
        Object payload = mode == 1 ? null : mode == 2 ? new Object() : new Value(7);
        Object replacement = new Value(9);
        RuntimeException original = new RuntimeException("owned allocation failure");
        Trace.payload = payload; Trace.replacement = replacement; Trace.failure = original;
        Trace.initMode = mode == 4 ? 1 : mode == 5 ? 2 : 0; Trace.failBody = mode == 3;
        for(int attempt = 0; attempt < 2; attempt++) {
            Trace.value = 0; Trace.after = null; Trace.seen = null; Trace.tail = 0;
            Trace.floatBits = 0; Trace.doubleBits = 0;
            Target result = null; Throwable failure = null;
            try { result = make(route); } catch(Throwable error) { failure = error; }
            if(mode == 4 || mode == 5) {
                require(result == null && Trace.seen == null && Trace.after == null);
                require(Trace.providerInit == (mode == 5));
                require(Trace.value == (attempt == 0 ? (mode == 4 ? 1 : 12) : 0));
                if(attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == original);
                else require(failure instanceof NoClassDefFoundError);
            } else if(mode == 2) {
                require(failure instanceof ClassCastException && Trace.seen == null && result == null && Trace.after == null);
                require(Trace.value == (attempt == 0 ? 12 : 0));
            } else {
                require(Trace.value == (attempt == 0 ? 123 : 3));
                Object expected = mode == 3 || attempt == 0 ? payload : replacement;
                require(Trace.seen instanceof Target); Target seen = (Target) Trace.seen;
                require(seen.first == input && seen.second == input && seen.mode == 2 && seen.value == expected && seen.alias == expected);
                require(seen.bits == bits && Float.floatToRawIntBits(seen.floating) == floatBits
                        && Double.doubleToRawLongBits(seen.fraction) == doubleBits);
                if(mode == 3) require(failure == original && result == null && Trace.after == null && Trace.tail == 0);
                else require(failure == null && result == seen && Trace.after == expected && Trace.tail == bits
                        && Trace.floatBits == floatBits && Trace.doubleBits == doubleBits);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.value);
        }
    }
}
