import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AbstractMarkerProducerHarness {
    static int shape, mode;
    static boolean derived;
    static Base subject;
    static Marker wanted, initialSaved;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned abstract marker producer"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Other implements Marker {}
    static final class Observer extends Trace {
        public void observe(int phase, Base actual) {
            check(actual == subject); events += phase + ",";
            if (phase == 1 || phase == 4) {
                check(actual.saved == initialSaved);
                if (mode == 4) AbstractMarkerProducerHarness.<RuntimeException>raise(failure);
                if (mode == 6) Trace.value = wanted = Trace.alternative;
                if (mode == 11) Trace.value = wanted = null;
            } else if (phase == 2 || phase == 5) {
                check(actual.saved == wanted);
                if (mode == 7) Trace.value = Trace.alternative;
                if (mode == 10) actual.saved = Trace.alternative;
                if (mode == 5) AbstractMarkerProducerHarness.<RuntimeException>raise(failure);
            } else {
                check(phase == 3);
                if (mode == 8) AbstractMarkerProducerHarness.<RuntimeException>raise(failure);
                if (mode == 9) Trace.value = Trace.alternative;
            }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return AbstractMarkerProducerCaller.call(subject, shape);
        String name = new String[]{"call", "object", "discard", "checked"}[shape];
        Class<?> returns = shape == 0 ? Marker.class : shape == 1 ? Object.class : shape == 2 ? void.class : Token.class;
        if (route == 1) {
            try { return Core.class.getMethod(name, Base.class).invoke(null, subject); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name, MethodType.methodType(returns, Base.class)).invokeWithArguments(subject);
        if (shape == 0) return Core.call(subject); if (shape == 1) return Core.object(subject);
        if (shape == 2) { Core.discard(subject); return null; } return Core.checked(subject);
    }
    static void metadata() throws Exception {
        for (Class<?> owner : new Class<?>[]{Base.class, Subject.class, Alternate.class}) {
            Method method = owner.getDeclaredMethod("read"); TypeVariable<?>[] formals = method.getTypeParameters();
            check((owner == Base.class ? Modifier.isProtected(method.getModifiers()) : Modifier.isPublic(method.getModifiers())) && !Modifier.isFinal(method.getModifiers()) && !Modifier.isStatic(method.getModifiers()));
            check(Modifier.isAbstract(method.getModifiers()) == (owner == Base.class));
            check(formals.length == 1 && formals[0].getBounds()[0] == Marker.class && method.getGenericReturnType().equals(formals[0]));
        }
        Method relay = Base.class.getDeclaredMethod("relay", String.class, Base.class);
        TypeVariable<?> formal = relay.getTypeParameters()[0];
        check(formal.getBounds()[0] == Marker.class && relay.getGenericReturnType().equals(formal));
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]); derived = Integer.parseInt(args[3]) != 0; metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Base original = derived ? new Alternate() : new Subject(); initialSaved = original.saved = new Token(); subject = mode == 1 ? null : original;
            wanted = mode == 2 ? null : mode == 3 ? new Other() : new Token();
            Trace.value = wanted; Trace.alternative = new Token(); Trace.hook = new Observer(); events = "";
            failure = mode == 4 || mode == 8 ? new Checked() : new LinkageError("owned failure");
            Object result = null; Throwable caught = null; try { result = call(route); } catch (Throwable problem) { caught = problem; }
            String expected = mode == 1 ? "" : (derived ? "4," : "1,") + (mode == 4 ? "" : derived ? "5," : "2,") + (mode == 4 || mode == 5 || shape != 3 ? "" : "3,");
            check(events.equals(expected));
            check(original.saved == (mode == 1 || mode == 4 ? initialSaved : mode == 10 ? Trace.alternative : wanted));
            if (mode == 1) check(caught instanceof NullPointerException);
            else if (mode == 4 || mode == 5 || (mode == 8 && shape == 3)) check(caught == failure);
            else if (mode == 3 && shape == 3) check(caught instanceof ClassCastException);
            else check(caught == null && result == (shape == 2 ? null : wanted));
        }
        System.out.println("attempts=2,abstract-marker-producer=matched");
    }
}
