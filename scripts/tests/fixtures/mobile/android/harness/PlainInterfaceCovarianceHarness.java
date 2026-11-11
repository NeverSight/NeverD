import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class PlainInterfaceCovarianceHarness {
    static int calls, failures;
    static void check(boolean value) {
        if (!value) throw new AssertionError("plain interface covariance contract");
    }
    static Method bridge(boolean wide) throws Exception {
        for (Method m : Worker.class.getDeclaredMethods()) {
            if (m.getName().equals("update") && m.getReturnType() == Root.class
                    && m.getParameterCount() == (wide ? 4 : 1)) {
                check(m.isBridge() && m.isSynthetic());
                return m;
            }
        }
        throw new AssertionError("missing original covariance bridge");
    }
    static Object call(int route, boolean wide, Worker w, Root delegate, int code,
                       Object value, Object[] array, long bits, double real) throws Throwable {
        if (delegate != null) {
            if (route == 0) return delegate.update(code,array,bits,real);
            if (route == 1) return ((Api)delegate).update(code,array,bits,real);
            if (route == 2 || route == 3) {
                Method entry = null;
                for (Method m : (route==2 ? Api.class : Root.class).getDeclaredMethods())
                    if (m.getName().equals("update") && m.getParameterCount()==4 && m.getReturnType()==Root.class) entry=m;
                check(entry!=null);
                if (route==2) check(entry.isBridge() && entry.isSynthetic() && entry.isDefault());
                try { return entry.invoke(delegate,code,array,bits,real); }
                catch (InvocationTargetException e) { throw e.getCause(); }
            }
            MethodHandle entry = MethodHandles.lookup().findVirtual(Api.class,"update",
                MethodType.methodType(Root.class,int.class,Object[].class,long.class,double.class));
            return (Root)entry.invokeExact((Api)delegate,code,array,bits,real);
        }
        if (route == 0) return wide ? w.update(code,array,bits,real) : w.update(value);
        if (route == 1) return wide ? ((Root)w).update(code,array,bits,real) : ((Root)w).update(value);
        if (route == 2) return wide ? ((Api)w).update(code,array,bits,real) : ((Api)w).update(value);
        if (route == 3) {
            try { return wide ? bridge(true).invoke(w,code,array,bits,real)
                              : bridge(false).invoke(w,value); }
            catch (InvocationTargetException e) { throw e.getCause(); }
        }
        MethodType mt = wide ? MethodType.methodType(Root.class,int.class,Object[].class,long.class,double.class)
                             : MethodType.methodType(Root.class,Object.class);
        MethodHandle mh = MethodHandles.lookup().findVirtual(Worker.class,"update",mt);
        return wide ? (Root)mh.invokeExact(w,code,array,bits,real) : (Root)mh.invokeExact(w,value);
    }
    static void fields() throws Throwable {
        for (Class<?> owner : new Class<?>[]{Root.class,Api.class}) {
            int expected = owner==Root.class ? 11 : 23;
            String label = owner==Root.class ? "owned-root" : "owned-api";
            Field flags = owner.getDeclaredField("FLAGS");
            check(flags.getDeclaringClass()==owner && flags.getInt(null)==expected);
            check(owner.getDeclaredField("LABEL").get(null)==label);
            MethodHandle read = MethodHandles.lookup().findStaticGetter(owner,"FLAGS",int.class);
            check((int)read.invokeExact()==expected);
            MethodHandle text = MethodHandles.lookup().findStaticGetter(owner,"LABEL",String.class);
            check((String)text.invokeExact()==label);
        }
        check(Worker.class.getField("FLAGS").getDeclaringClass()==Api.class);
    }
    public static void main(String[] args) throws Throwable {
        if (args.length!=0 && args[0].startsWith("fields")) fields();
        for (boolean wide : new boolean[]{false,true})
            for (int route=0;route<5;route++)
                for (boolean derived : new boolean[]{false,true})
                    for (boolean viaDefault : new boolean[]{false,true}) {
                        if (viaDefault && (!wide || args.length==0 || !args[0].contains("interface"))) continue;
                        for (int mode=0;mode<(wide?5:4);mode++) {
                        Worker w = derived ? new PlainInterfaceCovarianceDerived() : new Worker();
                        Root delegate = viaDefault ? new PlainInterfaceCovarianceViaDefault(w) : null;
                        if (viaDefault) {
                            for (Method m : delegate.getClass().getDeclaredMethods())
                                check(!(m.getName().equals("update") && m.getParameterCount()==4 && m.getReturnType()==Root.class));
                        }
                        Worker replacement = new Worker();
                        Object[] shared = new Object[]{new Object()};
                        for (int attempt=0;attempt<2;attempt++) {
                            Object value = !wide && mode==3 ? null : new Object();
                            shared[0] = value;
                            Object[] array = wide && mode==3 ? null : wide && mode==4 ? new Object[0] : shared;
                            int code = -37+attempt;
                            long bits = attempt==0 ? 0x12345678fedcba98L : 0xfedcba9876543210L;
                            double real = attempt==0 ? Double.longBitsToDouble(0x7ff8000000001234L) : -0.0d;
                            w.replacement = mode==1 ? null : replacement;
                            Object before = w.seen;
                            RuntimeException failure = new IllegalStateException("owned-failure");
                            Trace.failure = mode==2 ? failure : null;
                            Trace.events=0; Trace.subclassCalls=0; Trace.defaultCalls=0; Trace.parentDefaultCalls=0;
                            Object result=null; Throwable caught=null;
                            try { result=call(route,wide,w,delegate,code,value,array,bits,real); }
                            catch (Throwable e) { caught=e; }
                            calls++;
                            boolean badArray=wide && mode>=3;
                            if (badArray) {
                                check(caught!=null && caught.getClass()==(mode==3 ? NullPointerException.class : ArrayIndexOutOfBoundsException.class));
                                check(w.seen==before); failures++;
                            } else if (mode==2) {
                                check(caught==failure && w.seen==value); failures++;
                            } else {
                                check(caught==null && result==w.replacement && w.seen==value);
                            }
                            check(w.calls==attempt+1 && ((Root)w).scalar()==attempt+1);
                            check(Trace.subclassCalls==(derived?1:0));
                            check(Trace.defaultCalls==(viaDefault?1:0));
                            check(Trace.parentDefaultCalls==0);
                            if (viaDefault) check(delegate.scalar()==attempt+1);
                            check(Trace.events==(wide ? (caught==null?12:1) : (caught==null?34:3)));
                            if (wide) check(w.array==array && w.code==code && w.wide==bits
                                && Double.doubleToRawLongBits(w.real)==Double.doubleToRawLongBits(real));
                        }
                    }
                    }
        boolean interfaceBridge = args.length!=0 && args[0].contains("interface");
        check(calls==(interfaceBridge?280:180) && failures==(interfaceBridge?140:80));
        if (args.length!=0 && args[0].startsWith("fields")) fields();
        System.out.println("calls="+calls+",failures="+failures+",original-bridges-and-subclass-dispatch=matched");
    }
}
