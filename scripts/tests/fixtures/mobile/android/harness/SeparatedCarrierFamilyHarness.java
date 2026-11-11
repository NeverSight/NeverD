import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SeparatedCarrierFamilyHarness {
    static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned separate carrier family");
    }
    static void send(int route, WideAction action, long first, long second, String name) throws Throwable {
        try {
            if (route == 0) action.getClass().getMethod(name, long.class, long.class).invoke(action, first, second);
            else if (route == 1) WideOwner.call(action, first, second);
            else if (route == 2) WideAction.class.getMethod(name, long.class, long.class).invoke(action, first, second);
            else MethodHandles.publicLookup().findVirtual(WideAction.class, name,
                    MethodType.methodType(void.class, long.class, long.class)).invokeExact(action, first, second);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        String name = args[3];
        Method root = WideAction.class.getMethod(name, long.class, long.class);
        Method implementation = WideOwner.class.getMethod(name, long.class, long.class);
        require(root.getReturnType() == void.class && Modifier.isAbstract(root.getModifiers()));
        require(implementation.getReturnType() == void.class && Modifier.isFinal(implementation.getModifiers()));
        WideOwner target = new WideOwner();
        RuntimeException original = new IllegalArgumentException("owned wide action failure");
        target.failure = mode == 1 ? original : null;
        target.second = 17;
        long[] edges = {Long.MIN_VALUE, Long.MAX_VALUE, 0, -1, 1, 0x100000000L,
                        0x80000000L, 0x7fffffffL, -0x100000001L};
        for (int attempt = 0; attempt < 2; attempt++) {
            long first = edges[(mode + attempt) % edges.length];
            long second = edges[(mode + attempt + 1) % edges.length];
            try {
                send(route, mode == 2 ? null : target, first, second, name);
                require(mode != 1 && mode != 2);
            } catch (Throwable failure) {
                require(mode == 1 ? failure == original : mode == 2 && failure instanceof NullPointerException);
            }
            require(target.calls == (mode == 2 ? 0 : attempt + 1));
            require(target.first == (mode == 2 ? 0 : first));
            require(target.second == (mode == 1 || mode == 2 ? 17 : second));
        }
        CarrierTargetNamesHarness.main(new String[] {args[0], args[1], args[2]});
    }
}
