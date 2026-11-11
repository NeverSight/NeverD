import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InterfaceAllocationInitializerHarness {
    static int failPhase;
    static Throwable failure;
    static String events;
    static Api first, second, third;
    static void check(boolean value) { if (!value) throw new AssertionError("owned interface allocation initializer"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Observer extends Trace {
        public void observe(int phase, Api object) {
            check(Api.zFirst == (phase >= 3 ? first : null));
            check(Api.aSecond == (phase >= 5 ? second : null));
            check(Api.mThird == null);
            if (phase == 2) first = object;
            else if (phase == 4) second = object;
            else if (phase == 6) third = object;
            else check(object == null);
            events += phase + ",";
            if (phase == failPhase) InterfaceAllocationInitializerHarness.<RuntimeException>raise(failure);
        }
    }
    static Object get(int route, int index) throws Throwable {
        if (route == 0) return Core.get(index);
        if (route == 1) return InterfaceAllocationInitializerCaller.get(index);
        String name = index == 0 ? "zFirst" : index == 1 ? "aSecond" : "mThird";
        if (route == 2) return Api.class.getField(name).get(null);
        return MethodHandles.publicLookup().findStaticGetter(Api.class, name, Api.class).invokeWithArguments();
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), index = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        for (String name : new String[]{"zFirst", "aSecond", "mThird"}) {
            Field field = Api.class.getField(name); check(field.getType() == Api.class && Modifier.isStatic(field.getModifiers()) && Modifier.isFinal(field.getModifiers()));
        }
        failPhase = mode == 0 ? -1 : (mode - 1) % 6 + 1;
        failure = mode >= 7 ? new LinkageError("owned error") : new IllegalStateException("owned failure");
        Trace.hook = new Observer(); events = "";
        for (int attempt = 0; attempt < 2; attempt++) {
            String before = events; Object result = null; Throwable caught = null;
            try { result = get(route, index); } catch (Throwable problem) { caught = problem; }
            if (mode == 0) {
                check(caught == null && events.equals("1,2,3,4,5,6,"));
                check(first != null && second != null && third != null && first != second && second != third && first != third);
                check(Api.zFirst == first && Api.aSecond == second && Api.mThird == third);
                check(result == (index == 0 ? first : index == 1 ? second : third));
                check(((Api)result).id() == index + 1);
            } else if (attempt == 0) {
                String wanted = ""; for (int phase = 1; phase <= failPhase; phase++) wanted += phase + ",";
                check(result == null && events.equals(wanted));
                if (mode >= 7) check(caught == failure);
                else check(caught instanceof ExceptionInInitializerError && caught.getCause() == failure);
            } else check(result == null && caught instanceof NoClassDefFoundError && events.equals(before));
        }
        System.out.println("attempts=2,interface-allocation-initializer=matched");
    }
}
