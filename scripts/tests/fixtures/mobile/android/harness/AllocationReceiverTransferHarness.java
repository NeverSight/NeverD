import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationReceiverTransferHarness {
    static int mode, attempt, initializations, constructors;
    static Owner owner;
    static Object first, second, third, fourth;
    static boolean option, captured;
    static Throwable failure;
    static String events;
    static void check(boolean value) { if (!value) throw new AssertionError("owned allocation receiver transfer"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Observer extends Trace {
        public void observe(int phase, Target target) {
            events += phase + ",";
            if (phase == 1) {
                initializations++; check(target == null);
                if (mode == 3 || mode == 4) AllocationReceiverTransferHarness.<RuntimeException>raise(failure);
                if (owner != null) owner.flag = !owner.flag;
                captured = owner != null && owner.flag;
            } else {
                constructors++; check(target.first == first && target.second == second && target.third == third && target.fourth == fourth);
                check(target.option == option && target.captured == captured);
                Trace.last = target;
                if (owner != null) owner.flag = !owner.flag;
                if (mode == 11) target.option = !target.option;
                if (mode == 12) target.captured = !target.captured;
                if (mode == 13) Trace.owner = new Owner();
                if (mode == 5 || mode == 6 || mode == 14) AllocationReceiverTransferHarness.<RuntimeException>raise(failure);
            }
        }
    }
    static Class<?>[] parameters() {
        return new Class<?>[]{Object.class,Object.class,Object.class,Object.class,boolean.class,
            int.class,int.class,int.class,int.class,int.class,int.class,int.class,int.class,int.class,int.class,int.class,Owner.class};
    }
    static Object[] arguments() {
        return new Object[]{first,second,third,fourth,option,0,1,2,3,4,5,6,7,8,9,10,owner};
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return Core.make(first,second,third,fourth,option,0,1,2,3,4,5,6,7,8,9,10,owner);
        if (route == 1) {
            try { return Core.class.getMethod("make", parameters()).invoke(null, arguments()); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, "make", MethodType.methodType(Target.class, parameters())).invokeWithArguments(arguments());
        return AllocationReceiverTransferCaller.call(first,second,third,fourth,option,owner);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); option = Integer.parseInt(args[1]) != 0; mode = Integer.parseInt(args[2]);
        Constructor<?> ctor = Target.class.getConstructor(Object.class,Object.class,Object.class,Object.class,boolean.class,boolean.class);
        check(ctor.getParameterTypes().length == 6 && Core.class.getMethod("make", parameters()).getReturnType() == Target.class);
        for (attempt = 0; attempt < 2; attempt++) {
            Owner original = new Owner(); original.flag = mode == 2 || mode == 9 || mode == 15;
            owner = mode == 1 ? null : original; captured = owner != null && owner.flag;
            first = mode == 7 ? null : mode == 10 ? owner : new Object(); second = mode == 15 ? null : new Object();
            third = new Object(); fourth = mode == 8 ? null : new Object();
            Trace.owner = owner; Trace.last = null; Trace.hook = new Observer(); events = ""; initializations = constructors = 0;
            failure = mode == 3 || mode == 5 ? new Checked() : mode == 4 || mode == 6 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean failedInitialization = mode == 3 || mode == 4;
            boolean constructed = !failedInitialization && mode != 1;
            check(initializations == (attempt == 0 ? 1 : 0) && constructors == (constructed ? 1 : 0));
            check(events.equals((attempt == 0 ? "1," : "") + (constructed ? "2," : "")));
            if (failedInitialization) {
                if (attempt != 0) check(caught instanceof NoClassDefFoundError);
                else if (mode == 4) check(caught == failure);
                else check(caught instanceof ExceptionInInitializerError && caught.getCause() == failure);
            } else if (mode == 1) check(caught instanceof NullPointerException && Trace.last == null);
            else {
                check(Trace.last != null && Trace.last.option == (mode == 11 ? !option : option)
                    && Trace.last.captured == (mode == 12 ? !captured : captured));
                if (mode == 5 || mode == 6 || mode == 14) check(caught == failure && result == null);
                else check(caught == null && result == Trace.last);
            }
        }
        System.out.println("attempts=2,allocation-receiver-transfer=matched");
    }
}
