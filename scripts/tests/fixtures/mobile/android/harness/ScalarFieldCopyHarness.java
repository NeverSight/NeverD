import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ScalarFieldCopyHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("scalar field copy changed"); }
    static Target make(int route, Provider owner, int input, long bits, float floating, double fraction) throws Throwable {
        boolean statik = route >= 4; int entry = route % 4;
        String name = statik ? "makeStatic" : "make";
        if (entry == 0) return statik ? Core.makeStatic(owner,input,bits,floating,fraction) : Core.make(owner,input,bits,floating,fraction);
        if (entry == 1) return statik ? ScalarFieldCopyCaller.makeStatic(owner,input,bits,floating,fraction) : ScalarFieldCopyCaller.make(owner,input,bits,floating,fraction);
        if (entry == 2) {
            try { return (Target)Core.class.getMethod(name,Provider.class,int.class,long.class,float.class,double.class)
                    .invoke(null,owner,input,bits,floating,fraction); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Target)MethodHandles.publicLookup().findStatic(Core.class,name,
            MethodType.methodType(Target.class,Provider.class,int.class,long.class,float.class,double.class))
            .invokeExact(owner,input,bits,floating,fraction);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean statik = route >= 4, initial = (mode & 1) != 0;
        Provider owner = mode == 1 || mode == 8 ? null : new Provider();
        if (owner != null) owner.value = initial;
        Provider.staticFlag = initial;
        int input = mode == 3 ? Integer.MIN_VALUE : mode == 4 ? Integer.MAX_VALUE : -19;
        long bits = (mode & 1) == 0 ? 0x7ff8000000000031L : 0x8000000000000007L;
        int floatBits = mode == 5 ? 0x7f800000 : mode == 6 ? 0xff800000 : (mode & 1) == 0 ? 0x7fc01234 : 0x80000000;
        long doubleBits = mode == 5 ? 0x7ff0000000000000L : mode == 6 ? 0xfff0000000000000L : (mode & 1) == 0 ? 0x7ff8000000000057L : 0x8000000000000000L;
        RuntimeException original = new RuntimeException("owned scalar copy failure");
        Trace.owner = owner; Trace.failure = original; Trace.initMode = mode == 7 ? 1 : 0;
        Trace.failBody = mode == 2 || mode == 8;
        for (int attempt = 0; attempt < 2; attempt++) {
            boolean expectedFlag = attempt == 0 ? !initial : statik ? Provider.staticFlag : owner != null && owner.value;
            Trace.value = 0; Trace.seen = null; Trace.afterWritten = false; Trace.afterFlag = !expectedFlag;
            Trace.tail = 0; Trace.floatBits = 0; Trace.doubleBits = 0;
            Throwable failure = null; Target result = null;
            try { result = make(route,owner,input,bits,Float.intBitsToFloat(floatBits),Double.longBitsToDouble(doubleBits)); }
            catch (Throwable error) { failure = error; }
            if (mode == 7) {
                require(result == null && Trace.seen == null && !Trace.afterWritten && Trace.value == (attempt == 0 ? 1 : 0));
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == original);
                else require(failure instanceof NoClassDefFoundError);
            } else if (owner == null && !statik) {
                require(failure instanceof NullPointerException && result == null && Trace.seen == null && !Trace.afterWritten);
                require(Trace.value == (attempt == 0 ? 1 : 0));
            } else {
                Target seen = Trace.seen;
                require(seen != null && seen.first == expectedFlag && seen.second == expectedFlag && seen.input == input && seen.alias == input);
                require(seen.bits == bits && Float.floatToRawIntBits(seen.floating) == floatBits && Double.doubleToRawLongBits(seen.fraction) == doubleBits);
                require(Trace.value == (attempt == 0 ? 13 : 3));
                if (Trace.failBody) require(failure == original && result == null && !Trace.afterWritten && Trace.tail == 0);
                else require(failure == null && result == seen && Trace.afterWritten && Trace.afterFlag == expectedFlag
                    && Trace.tail == bits && Trace.floatBits == floatBits && Trace.doubleBits == doubleBits);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.value);
        }
    }
}
