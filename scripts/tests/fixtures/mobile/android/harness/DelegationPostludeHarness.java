import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class DelegationPostludeHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("delegation postlude contract"); }
    static Core create(int level, int route, Object value, int mode, long bits) throws Throwable {
        try {
            if (route == 0) {
                if (level == 0) return new Core(value);
                if (level == 1) return new Core(value, mode);
                return new Core(value, mode, bits);
            }
            if (route == 1) {
                if (level == 0) return DelegationPostludeCaller.create(value);
                if (level == 1) return DelegationPostludeCaller.create(value, mode);
                return DelegationPostludeCaller.create(value, mode, bits);
            }
            Class<?>[] types = level == 0 ? new Class<?>[] {Object.class}
                : level == 1 ? new Class<?>[] {Object.class, int.class}
                : new Class<?>[] {Object.class, int.class, long.class};
            if (route == 2) {
                Object[] values = level == 0 ? new Object[] {value}
                    : level == 1 ? new Object[] {value, mode} : new Object[] {value, mode, bits};
                return Core.class.getConstructor(types).newInstance(values);
            }
            MethodHandle ctor = MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class, types));
            if (level == 0) return (Core) ctor.invokeExact(value);
            if (level == 1) return (Core) ctor.invokeExact(value, mode);
            return (Core) ctor.invokeExact(value, mode, bits);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int level = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.failAt = mode;
        Trace.failure = new IllegalArgumentException("owned postlude failure");
        Core previous = null;
        long expected = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Object value = attempt == 1 ? new int[] {7} : null;
            int mark = attempt == 1 ? Integer.MIN_VALUE : Integer.MAX_VALUE;
            long stamp = attempt == 1 ? Long.MAX_VALUE : Long.MIN_VALUE;
            Trace.expected = value; Trace.expectedMark = mark; Trace.expectedStamp = stamp;
            Throwable caught = null;
            try {
                Core result = create(level, route, value, mark, stamp);
                require(mode == 0 && result != previous && result == Trace.current && result.base == value
                        && result.mark == (level >= 1 ? mark : 0) && result.stamp == (level >= 2 ? stamp : 0));
                previous = result;
            } catch (Throwable failure) { caught = failure; }
            require(mode == 0 ? caught == null : caught == Trace.failure);
            if (mode == 1) require(Trace.current == null);
            else require(Trace.current.base == value && Trace.current.mark == (level >= 1 && (mode == 0 || mode >= 3) ? mark : 0)
                         && Trace.current.stamp == (level >= 2 && (mode == 0 || mode >= 4) ? stamp : 0));
            for (int event = 1; event <= (mode == 0 ? level + 2 : mode); event++) expected = expected * 10 + event;
            require(Trace.events == expected);
        }
        System.out.println("level=" + level + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
