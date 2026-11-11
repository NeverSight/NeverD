package probe;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class BranchedInterfaceSuperAccessorHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned branched interface super accessor mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean future = Boolean.parseBoolean(args[2]), rebuilt = Boolean.parseBoolean(args[3]);
        require(Arrays.equals(Child.class.getInterfaces(), new Class<?>[]{Side.class, Parent.class}));
        require(Arrays.equals(Parent.class.getInterfaces(), new Class<?>[]{Root.class}));
        require(Child.class.getTypeParameters().length == 0 && Parent.class.getTypeParameters().length == 0);
        Method accessor = Child.class.getDeclaredMethod("access$measure$jd", Child.class, long.class);
        require(accessor.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC | (rebuilt ? 0 : 0x1000)));
        require(accessor.getReturnType() == int.class && accessor.getTypeParameters().length == 0);
        require(accessor.getExceptionTypes().length == 0);
        Method parent = Parent.class.getDeclaredMethod("measure", long.class);
        require(parent.isDefault() && parent.getModifiers() == Modifier.PUBLIC && parent.getReturnType() == int.class);
        require(parent.getExceptionTypes().length == 0 && parent.getTypeParameters().length == 0);
        require(Arrays.stream(Parent.class.getDeclaredMethods()).noneMatch(m -> m.getName().equals("access$measure$jd")));
        require(Root.class.getDeclaredMethod("read", long.class).getModifiers() == (Modifier.PUBLIC | Modifier.ABSTRACT));
        require(Side.class.getDeclaredMethod("side").getModifiers() == (Modifier.PUBLIC | Modifier.ABSTRACT));
        Method compatibility = Child.DefaultImpls.class.getMethod("measure", Child.class, long.class);
        MethodType type = MethodType.methodType(int.class, Child.class, long.class);
        MethodHandle original = MethodHandles.publicLookup().findStatic(Child.class, accessor.getName(), type);
        MethodHandle compatible = MethodHandles.publicLookup().findStatic(Child.DefaultImpls.class, "measure", type);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Receiver receiver = mode == 1 ? null : future ? new BranchedInterfaceSuperAccessorCaller.Future() : new Receiver();
            final Receiver nested = new Receiver();
            final long bits = attempt == 0 ? Long.MIN_VALUE + mode : 0x123456789abcdef0L + mode;
            final Throwable chosen = mode == 2 || mode == 5 || mode == 8 ? new java.io.IOException()
                    : mode == 4 || mode == 7 ? new AssertionError() : new IllegalStateException();
            final int[] counts = {0, 0, 0};
            Trace.events = 0;
            Trace.bits = Trace.input = 0;
            Trace.parent = Trace.receiver = null;
            Trace.overrides = 0;
            Trace.answer = attempt == 0 ? Integer.MIN_VALUE + mode : 0x1234abcd + mode;
            Trace.other = ~Trace.answer;
            Trace.last = 0;
            Trace.action = new Runnable() { public void run() {
                int call = ++counts[0];
                require(call == 1 || mode == 10 && call == 2);
                require(Trace.parent == (call == 2 ? nested : receiver));
                require(Trace.bits == (call == 2 ? ~bits : bits));
                if (mode >= 2 && mode <= 4) BranchedInterfaceSuperAccessorHarness.<RuntimeException>raise(chosen);
                if (mode == 9) Trace.answer = ~Trace.answer;
                if (mode == 10 && call == 1)
                    require(Child.DefaultImpls.measure(nested, ~bits) == Trace.answer);
            }};
            Trace.readAction = mode == 11 ? null : new Runnable() { public void run() {
                int call = ++counts[1];
                boolean inner = mode == 10 && call == 1;
                require(call == 1 || mode == 10 && call == 2);
                require(Trace.receiver == (inner ? nested : receiver));
                require(Trace.input == (inner ? ~bits : bits));
                if (mode == 5 || mode == 6) BranchedInterfaceSuperAccessorHarness.<RuntimeException>raise(chosen);
            }};
            Trace.afterAction = new Runnable() { public void run() {
                int call = ++counts[2];
                require(call == 1 || mode == 10 && call == 2);
                require(Trace.last == Trace.answer);
                if (mode == 7 || mode == 8) BranchedInterfaceSuperAccessorHarness.<RuntimeException>raise(chosen);
            }};
            int result = 0;
            Throwable failure = null;
            try {
                if (route == 0) result = Core.call(receiver, bits);
                else if (route == 1) result = BranchedInterfaceSuperAccessorCaller.call(receiver, bits);
                else {
                    Trace.hit(6);
                    if (route == 2) result = Child.DefaultImpls.measure(receiver, bits);
                    else if (route == 3 || route == 5) {
                        try { result = (Integer) (route == 3 ? accessor : compatibility).invoke(null, receiver, bits); }
                        catch (InvocationTargetException caught) { throw caught.getCause(); }
                    } else result = (int) (route == 4 ? original : compatible).invokeExact((Child) receiver, bits);
                    Trace.hit(0);
                }
            } catch (Throwable caught) { failure = caught; }
            boolean entered = receiver != null;
            boolean read = entered && !(mode >= 2 && mode <= 4);
            boolean readDone = read && mode != 5 && mode != 6 && mode != 11;
            boolean completed = readDone && mode != 7 && mode != 8;
            String sequence = "6";
            if (entered) sequence += "1";
            if (mode == 10) sequence += "12345";
            if (read) sequence += (future ? "7" : "") + "2";
            if (readDone) sequence += "3" + (future ? "8" : "") + "4";
            if (completed) sequence += "50";
            require(Trace.events == Long.parseLong(sequence));
            require(Trace.overrides == 0);
            require(counts[0] == (entered ? mode == 10 ? 2 : 1 : 0));
            require(counts[1] == (read && mode != 11 ? mode == 10 ? 2 : 1 : 0));
            require(counts[2] == (readDone ? mode == 10 ? 2 : 1 : 0));
            require(Trace.parent == (entered ? mode == 10 ? nested : receiver : null));
            require(Trace.bits == (entered ? mode == 10 ? ~bits : bits : 0));
            require(Trace.receiver == (read ? receiver : null));
            require(Trace.input == (read ? bits : 0));
            require(Trace.last == (readDone ? Trace.answer : 0));
            if (!entered || mode == 11) require(failure instanceof NullPointerException);
            else if (!completed) require(failure == chosen);
            else require(failure == null && result == Trace.answer);
        }
        System.out.println("attempts=2,branched-interface-super-accessor=matched");
    }
}
