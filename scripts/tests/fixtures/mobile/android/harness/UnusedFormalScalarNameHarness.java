import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class UnusedFormalScalarNameHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("unused scalar formal name changed"); }
    static int call(int input, int route, String name) throws Throwable {
        if (route == 0) return Entry.call(input);
        if (route == 1) return UnusedFormalScalarNameCaller.call(input);
        if (route == 2) {
            try { return ((Integer) Entry.class.getMethod(name, int.class).invoke(null, input)).intValue(); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (int) MethodHandles.publicLookup().findStatic(Entry.class, name,
                MethodType.methodType(int.class, int.class)).invokeExact(input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        String name = args[2];
        Method method = Entry.class.getMethod(name, int.class);
        require(method.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC | Modifier.FINAL));
        require(!method.isSynthetic() && method.getTypeParameters().length == 1);
        require(method.getGenericReturnType() == int.class && method.getGenericParameterTypes()[0] == int.class);
        require(method.getTypeParameters()[0].getName().equals("T"));
        require(Provider.class.getTypeParameters().length == 1);
        require(Provider.class.getMethod("relay", int.class).getTypeParameters().length == 1);
        Trace.failAt = mode >= 1 && mode <= 3 ? mode : mode == 8 ? 5 : mode == 9 ? 4 : 0;
        Trace.failure = new RuntimeException("owned unused scalar failure");
        Trace.mask = 0xa5a5a5a5;
        Trace.values = new int[] {7};
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.values[0] = mode == 4 ? Integer.MIN_VALUE : mode == 5 ? Integer.MAX_VALUE
                    : mode == 6 ? -1 : mode == 7 ? 0x89abcdef : 7 + attempt;
            int input = Trace.values[0], sentinel = 0x12345678;
            Trace.events = 0; Trace.seen = sentinel; Trace.after = sentinel;
            Throwable failure = null; int result = sentinel;
            try { result = call(input, route, name); } catch (Throwable error) { failure = error; }
            boolean provider = mode != 8 && mode != 9;
            boolean read = provider && mode != 1 && mode != 2;
            int trace = attempt == 0 ? 5 : 0;
            if (mode != 8) trace = trace * 10 + 4;
            if (provider && attempt == 0) trace = trace * 10 + 1;
            if (provider && mode != 1) trace = trace * 10 + 2;
            if (read) trace = trace * 10 + 3;
            require(Trace.events == trace && Trace.seen == (read ? input : sentinel));
            int expectedInput = attempt == 0 && provider && mode != 1 ? 41
                    : attempt == 0 && mode != 8 ? -19 : input;
            require(Trace.values[0] == expectedInput);
            if (mode == 1 || mode == 8) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode == 2 || mode == 3 || mode == 9) require(failure == Trace.failure);
            else require(failure == null && result == (input ^ Trace.mask) && Trace.after == result);
            if (failure != null) require(Trace.after == sentinel);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
