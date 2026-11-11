import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class InterfaceSuperGettersHarness {
    static int calls, failures;
    public static class Derived extends Wrapper {
        @Override public Payload getStyle() { Trace.overrides++; return super.getStyle(); }
    }
    static void check(boolean value) {
        if (!value) throw new AssertionError("interface super getter contract");
    }
    static Payload call(int route, Wrapper receiver) throws Throwable {
        if (route == 0) return receiver.getStyle();
        if (route == 1) return ((Api)receiver).getStyle();
        if (route == 2) return ((Root)receiver).getStyle();
        if (route == 3) {
            try { return (Payload)Wrapper.class.getMethod("getStyle").invoke(receiver); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandle entry = MethodHandles.lookup().findVirtual(Wrapper.class,"getStyle",MethodType.methodType(Payload.class));
        return (Payload)entry.invokeExact(receiver);
    }
    public static void main(String[] args) throws Throwable {
        Method entry = Wrapper.class.getDeclaredMethod("getStyle");
        boolean original = args[0].equals("original");
        check(entry.getReturnType() == Payload.class && !Modifier.isFinal(entry.getModifiers()));
        check(entry.isBridge() == original && entry.isSynthetic() == (original && args[1].equals("synthetic")));
        for (int route=0;route<5;route++)
            for (boolean derived : new boolean[]{false,true})
                for (int mode=0;mode<7;mode++) {
                    Wrapper receiver = derived ? new Derived() : new Wrapper();
                    Payload[] shared = {new Payload(1),new Payload(2)};
                    Trace.calls=Trace.rootCalls=0;
                    for (int attempt=0;attempt<2;attempt++) {
                        Trace.index = mode==2 ? -1 : mode==3 ? 2 : attempt;
                        Payload expected = mode==1 ? null : shared[attempt];
                        shared[attempt]=expected;
                        Trace.items = mode==4 ? null : shared;
                        Trace.alternate=new Payload(999);
                        Trace.events=Trace.overrides=0;
                        RuntimeException failure = new IllegalStateException("owned-default-failure");
                        Trace.failure = mode>=2 ? failure : null;
                        Payload result=null; Throwable caught=null;
                        try { result=call(route, mode==6 ? null : receiver); }
                        catch (Throwable error) { caught=error; }
                        calls++;
                        if (mode>=2) {
                            failures++;
                            if (mode==5) check(caught==failure);
                            else check(caught!=null && caught.getClass()==(mode==2 || mode==3 ? ArrayIndexOutOfBoundsException.class : NullPointerException.class));
                        } else check(caught==null && result==expected);
                        check(Trace.calls==(mode==6 ? 0 : attempt+1));
                        check(Trace.rootCalls==0);
                        check(Trace.events==(mode==6 ? 0 : mode>=2 ? 1 : 12));
                        check(Trace.overrides==(derived && mode!=6 ? 1 : 0));
                    }
                }
        check(calls==140 && failures==100);
        System.out.println("calls="+calls+",failures="+failures+",original-interface-super-dispatch=matched");
    }
}
