import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class AnonymousCovariantHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("anonymous covariance contract");
    }
    static Object read(int route, Base self) throws Throwable {
        try {
            switch (route) {
            case 0: return Client.read(self);
            case 1: return AnonymousCovariantCaller.read(self);
            case 2: return Base.class.getDeclaredMethod("getItem").invoke(self);
            case 3:
            case 4:
                for (Method method : self.getClass().getDeclaredMethods()) {
                    if (!method.getName().equals("getItem") || method.getParameterCount() != 0
                            || method.getReturnType() != (route == 3 ? Menu.class : Owner.class)) continue;
                    check(method.isBridge() == (route == 3));
                    check(method.isSynthetic() == (route == 3));
                    method.setAccessible(true);
                    return method.invoke(self);
                }
                throw new AssertionError("anonymous covariance contract: missing original slot");
            default:
                return (Menu) MethodHandles.publicLookup().findVirtual(Base.class, "getItem",
                        MethodType.methodType(Menu.class)).invokeExact(self);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean absent = Integer.parseInt(args[2]) != 0;
        RuntimeException failure = new IllegalStateException("owned anonymous callback failure");
        Trace.failure = failure;
        Trace.failAt = mode;
        Owner first = new Owner(), second = new Owner();
        Object firstToken = absent ? null : new Object(), secondToken = new Object();
        Base[] values = new Base[2];
        for (int attempt = 0; attempt < 2; attempt++) {
            Throwable error = null;
            try { values[attempt] = (attempt == 0 ? first : second).create(attempt == 0 ? firstToken : secondToken); }
            catch (Throwable caught) { error = caught; }
            if (mode == 1) {
                check(values[attempt] == null && Trace.events == 1 && Trace.seenToken == null);
                check(error != null && error.getClass() == (attempt == 0 ? ExceptionInInitializerError.class : NoClassDefFoundError.class));
                if (attempt == 0) check(error.getCause() == failure);
            } else if (mode == 2) {
                check(values[attempt] == null && error == failure);
                check(Trace.events == (attempt == 0 ? 12 : 122));
                check(Trace.seenToken == (attempt == 0 ? firstToken : secondToken));
            } else {
                check(error == null && values[attempt] != null);
                check(values[attempt].token == (attempt == 0 ? firstToken : secondToken));
                check(Trace.events == (attempt == 0 ? 12 : 122));
                check(Trace.seenToken == (attempt == 0 ? firstToken : secondToken));
            }
        }
        if (mode != 1 && mode != 2) {
            check(values[0] != values[1]);
            int events = 122;
            for (int index = 0; index < 4; index++) {
                int selected = index & 1;
                Object result = null;
                Throwable error = null;
                try { result = read(route, values[selected]); } catch (Throwable caught) { error = caught; }
                events = events * 10 + 3;
                check(Trace.events == events && Trace.seenToken == secondToken);
                check(values[0].token == firstToken && values[1].token == secondToken);
                if (mode == 3) check(result == null && error == failure);
                else check(error == null && result == (selected == 0 ? first : second));
            }
        }
        System.out.println("route=" + route + ",mode=" + mode + ",absent=" + (absent ? 1 : 0) + ",trace=" + Trace.events);
    }
}
