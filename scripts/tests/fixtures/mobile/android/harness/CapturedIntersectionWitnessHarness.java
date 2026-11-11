package probe;
import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class CapturedIntersectionWitnessHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned captured intersection witness mismatch"); }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable error) throws E { throw (E)error; }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean future = Boolean.parseBoolean(args[2]), projected = Boolean.parseBoolean(args[3]);
        Class<?> member = Class.forName("fixture.Owner$Member");
        require(member.getEnclosingClass() == Owner.class && member.isMemberClass());
        require(member.getModifiers() == (Modifier.PRIVATE | (projected ? Modifier.STATIC : 0)));
        require(member.getTypeParameters().length == 1 && member.getTypeParameters()[0].getBounds()[0] == Object.class);
        require(member.getGenericSuperclass() instanceof ParameterizedType);
        ParameterizedType parent = (ParameterizedType)member.getGenericSuperclass();
        require(parent.getRawType() == Base.class && parent.getActualTypeArguments()[0] == String.class);
        Method getter = member.getDeclaredMethod("owner"); getter.setAccessible(true);
        Method accept = Provider.class.getDeclaredMethod("accept", First.class);
        TypeVariable<?> formal = accept.getTypeParameters()[0];
        require(java.util.Arrays.equals(formal.getBounds(), new Type[]{First.class,Second.class}));
        require(accept.getGenericParameterTypes()[0].equals(formal) && accept.getExceptionTypes().length == 0);
        Method entry = Owner.class.getMethod("run", String[].class);
        MethodHandle handle = MethodHandles.lookup().findVirtual(Owner.class,"run",MethodType.methodType(void.class,String[].class));
        final Owner receiver = future ? new CapturedIntersectionWitnessCaller.Future() : new Owner();
        for (int attempt = 0; attempt < 2; ++attempt) {
            final String[] values = mode == 2 ? null : new String[]{"owned"};
            final Object context = mode == 1 ? null : new Object();
            final int index = 37 + attempt;
            final Throwable failure = mode == 3 || mode == 6 || mode == 9 ? new java.io.IOException()
                : mode == 5 || mode == 8 ? new AssertionError() : new IllegalStateException();
            final int[] calls = {0,0,0,0};
            receiver.index = index;
            Trace.events = 0; Trace.context = context; Trace.contextSeen = null;
            Trace.captured = null; Trace.receiver = null; Trace.argument = null; Trace.inputs = null; Trace.indexSeen = 0;
            Trace.contextAction = new Runnable() { public void run() {
                require(++calls[0] == 1 && Trace.events == 1);
                if (mode == 3) CapturedIntersectionWitnessHarness.<RuntimeException>raise(failure);
                if (mode == 10) receiver.index = 77;
            } };
            Trace.construct = new Runnable() { public void run() {
                require(++calls[1] == 1 && Trace.events == 12 && Trace.captured == receiver);
                require(Trace.contextSeen == context && Trace.inputs == values && Trace.indexSeen == (mode == 10 ? 77 : index));
                if (mode == 4 || mode == 5) CapturedIntersectionWitnessHarness.<RuntimeException>raise(failure);
            } };
            Trace.before = new Runnable() { public void run() {
                require(++calls[2] == 1 && Trace.receiver == receiver && Trace.argument.getClass() == member);
                try { require(getter.invoke(Trace.argument) == receiver); }
                catch (ReflectiveOperationException error) { throw new AssertionError(error); }
                if (mode == 6 || mode == 7) CapturedIntersectionWitnessHarness.<RuntimeException>raise(failure);
                receiver.index = -1; Trace.context = new Object();
                if (mode == 11) values[0] = "changed";
            } };
            Trace.action = new Runnable() { public void run() {
                require(++calls[3] == 1 && Trace.captured == receiver && Trace.inputs == values);
                require(Trace.indexSeen == (mode == 10 ? 77 : index) && Trace.contextSeen == context);
                if (mode == 11) require(values[0].equals("changed"));
                if (mode == 8 || mode == 9) CapturedIntersectionWitnessHarness.<RuntimeException>raise(failure);
            } };
            Throwable caught = null;
            try {
                if (route == 0) receiver.run(values);
                else if (route == 1) CapturedIntersectionWitnessCaller.call(receiver,values);
                else if (route == 2) try { entry.invoke(receiver,(Object)values); }
                     catch (InvocationTargetException wrapped) { throw wrapped.getCause(); }
                else handle.invokeExact(receiver,values);
            } catch (Throwable error) { caught = error; }
            String events = mode == 3 ? "1" : mode == 4 || mode == 5 ? "12" : "1234" + (future ? "9" : "")
                + (mode == 6 || mode == 7 ? "5" : mode == 8 || mode == 9 ? "56" : "5678");
            require(Trace.events == Long.parseLong(events));
            require(calls[0] == 1 && calls[1] == (mode == 3 ? 0 : 1));
            require(calls[2] == (mode >= 3 && mode <= 5 ? 0 : 1));
            require(calls[3] == (mode >= 3 && mode <= 7 ? 0 : 1));
            require(Trace.captured == (mode == 3 ? null : receiver));
            if (mode >= 3 && mode <= 9) require(caught == failure); else require(caught == null);
        }
        System.out.println("attempts=2,captured-intersection-witness=matched");
    }
}
