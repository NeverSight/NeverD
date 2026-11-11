import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public class NestedVisibilitySuperBridgeHarness {
    static final String[] NAMES = {"index", "flag", "wide", "ratio", "value", "touch", "run"};
    static final Class<?>[] RESULTS = {int.class, boolean.class, long.class, double.class, Object.class, void.class, boolean.class};
    public static class Future extends Outer.Facade {
        public int index() { Trace.hit(4); int result = super.index(); Trace.hit(5); return result; }
        public boolean flag() { Trace.hit(4); boolean result = super.flag(); Trace.hit(5); return result; }
        public long wide() { Trace.hit(4); long result = super.wide(); Trace.hit(5); return result; }
        public double ratio() { Trace.hit(4); double result = super.ratio(); Trace.hit(5); return result; }
        public Object value() { Trace.hit(4); Object result = super.value(); Trace.hit(5); return result; }
        public void touch(Context context, Outer input, long bits) { Trace.hit(4); super.touch(context,input,bits); Trace.hit(5); }
        public boolean run(Context context, Outer input, int bits) { Trace.hit(4); boolean result = super.run(context,input,bits); Trace.hit(5); return result; }
    }
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned nested visibility superclass bridge mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E)failure; }
    static Object call(Outer.Facade self, int route, int operation, Context context, Bound input, long bits) throws Throwable {
        if (route == 0) {
            switch (operation) {
            case 0: return self.index();
            case 1: return self.flag();
            case 2: return self.wide();
            case 3: return self.ratio();
            case 4: return self.value();
            case 5: self.touch(context,(Outer)input,bits); return null;
            case 6: return self.run(context,(Outer)input,(int)bits);
            default: throw new AssertionError();
            }
        }
        if (route == 1) return NestedVisibilitySuperBridgeCaller.call(self,operation,context,(Outer)input,bits);
        if (route == 4) { NestedVisibilitySuperBridgeCaller.raw(self,context,input,bits); return null; }
        Class<?>[] parameters = operation == 5 ? new Class<?>[]{Context.class,Bound.class,long.class}
            : operation == 6 ? new Class<?>[]{Context.class,Outer.class,int.class} : new Class<?>[]{};
        if (route == 2) {
            Method method = Outer.Facade.class.getDeclaredMethod(NAMES[operation],parameters);
            check(method.isBridge() && method.isSynthetic() && Modifier.isPublic(method.getModifiers()));
            try {
                if (operation == 5) return method.invoke(self,context,input,bits);
                if (operation == 6) return method.invoke(self,context,input,(int)bits);
                return method.invoke(self);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findVirtual(Outer.Facade.class,NAMES[operation],MethodType.methodType(RESULTS[operation],parameters));
        switch(operation) {
        case 0: return (int)handle.invokeExact(self);
        case 1: return (boolean)handle.invokeExact(self);
        case 2: return (long)handle.invokeExact(self);
        case 3: return (double)handle.invokeExact(self);
        case 4: return (Object)handle.invokeExact(self);
        case 5: handle.invokeExact(self,context,(Bound)input,bits); return null;
        case 6: return (boolean)handle.invokeExact(self,context,(Outer)input,(int)bits);
        default: throw new AssertionError();
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), operation=Integer.parseInt(args[1]), future=Integer.parseInt(args[2]), mode=Integer.parseInt(args[3]);
        final Outer.Facade self=future == 0 ? new Outer.Facade() : new Future();
        for (int attempt=0; attempt<2; ++attempt) {
            final Context context=attempt == 0 ? new Context() : null;
            final Bound input=attempt == 0 ? (mode == 4 ? new Bound() : new Outer()) : null;
            final long bits=attempt == 0 ? 0x76543210fedcba98L : Long.MIN_VALUE+17;
            final Object resultObject=attempt == 0 ? new Object() : null;
            final long doubleBits=attempt == 0 ? 0x7ff8000000000042L : 0x8000000000000000L;
            final Throwable failure=mode == 1 ? new IllegalStateException() : mode == 2 ? new AssertionError() : mode == 3 ? new Exception() : null;
            final int expectedPrefix=future == 0 ? 12 : 412;
            final int expectedOperation=operation;
            Trace.events=0; Trace.receiver=null; Trace.context=new Object(); Trace.input=new Bound(); Trace.bits=0;
            Trace.integer=41; Trace.flag=false; Trace.wide=42; Trace.ratio=43; Trace.value=new Object();
            Trace.action=new Runnable() { public void run() {
                check(Trace.events == expectedPrefix && Trace.receiver == self);
                if (expectedOperation >= 5) check(Trace.context == context && Trace.input == input && Trace.bits == (expectedOperation == 5 ? bits : (int)bits));
                Trace.hit(6); Trace.integer=(int)bits; Trace.flag=true; Trace.wide=bits;
                Trace.ratio=Double.longBitsToDouble(doubleBits); Trace.value=resultObject;
                if (input != null) input.mark=17;
                if (failure != null) NestedVisibilitySuperBridgeHarness.<RuntimeException>raise(failure);
            }};
            Object result=null; Throwable caught=null;
            try { result=call(self,route,operation,context,input,bits); } catch(Throwable error) { caught=error; }
            if (mode == 4 && future == 1 && input != null) {
                check(caught instanceof ClassCastException && Trace.events == 0 && input.mark == 0);
                System.out.println("route="+route+",operation="+operation+",future="+future+",mode="+mode+",attempt="+attempt+",trace=0");
                continue;
            }
            check(caught == failure);
            check(Trace.events == (failure == null ? (future == 0 ? 1263 : 412635) : (future == 0 ? 126 : 4126)));
            if (failure == null) {
                if (operation == 0) check(((Integer)result).intValue() == (int)bits);
                if (operation == 1 || operation == 6) check(Boolean.TRUE.equals(result));
                if (operation == 2) check(((Long)result).longValue() == bits);
                if (operation == 3) check(Double.doubleToRawLongBits(((Double)result).doubleValue()) == doubleBits);
                if (operation == 4) check(result == resultObject);
                if (operation == 5) check(result == null);
            }
            if (input != null) check(input.mark == 17);
            System.out.println("route="+route+",operation="+operation+",future="+future+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
