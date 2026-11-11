import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class MultiparamBoxedSuperclassHarness {
    static int values, identities, states, failures;
    static final Method get;
    static final MethodHandle handle, api;
    static { try {
        get = java.util.Arrays.stream(Core.class.getDeclaredMethods())
            .filter(m -> m.getName().equals("get") && m.getReturnType() == Object.class)
            .findFirst().orElseThrow(AssertionError::new);
        handle = MethodHandles.publicLookup().unreflect(get);
        api = MethodHandles.publicLookup().unreflect(Api.class.getMethod("at", Core.class));
    } catch (Exception error) { throw new AssertionError(error); } }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int route, Core self) throws Throwable {
        try { switch (route) {
        case 0: return Caller.call(self);
        case 1: return ((Base)self).parentGet();
        case 2: return ((Api)self).at(self);
        case 3: return get.invoke(self);
        case 4: return handle.invoke(self);
        default: return api.invoke(self,self);
        } } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        value(get.isBridge() && get.isSynthetic());
        value(((ParameterizedType)Core.class.getGenericSuperclass()).getActualTypeArguments()[1] == Box.class);
        value(Core.class.getDeclaredMethod("readValue").getReturnType() == byte[].class);
        Type[] types = ((ParameterizedType)Core.class.getGenericSuperclass()).getActualTypeArguments();
        value(types[0] == String.class && ((ParameterizedType)types[2]).getActualTypeArguments()[0] == Box.class
            && Modifier.isProtected(Core.class.getDeclaredMethod("readValue").getModifiers()));
        RuntimeException targetFailure = new IllegalStateException("owned target failure");
        RuntimeException factoryFailure = new IllegalArgumentException("owned factory failure");
        byte[][] arrays = {null, new byte[0], new byte[]{Byte.MIN_VALUE,0,Byte.MAX_VALUE}};
        for (int route = 0; route < 6; route++) for (byte[] data : arrays)
            for (int fail = 0; fail < 3; fail++) for (int mode = 0; mode < 3; mode++) {
                Core self = new Core(data); int[] marker = {0}; self.marker = Box.marker = marker;
                self.failure = fail == 1 ? targetFailure : null; Box.failure = fail == 2 ? factoryFailure : null;
                byte[] oldData = {42}, cacheData = {9}; Box sentinel = new Box(oldData), cached = new Box(cacheData);
                Box.last = sentinel; Box.lastInput = oldData; Box.observed = sentinel;
                Box.cached = mode == 1 ? cached : null; Box.returnNull = mode == 2;
                int before = Box.calls; Object result = null; Throwable error = null;
                try { result = call(route, self); } catch (Throwable actual) { error = actual; }
                same(self.observed, marker);
                state(self.calls == 1 && Box.calls == before + (fail == 1 ? 0 : 1) && marker[0] == (fail == 1 ? 3 : 8));
                if (fail == 1) { same(error, targetFailure); same(Box.last, sentinel); same(Box.lastInput, oldData); failures++; }
                else {
                    same(Box.lastInput, data); same(Box.observed, marker);
                    if (fail == 2) { same(error, factoryFailure); same(Box.last, sentinel); failures++; }
                    else {
                        same(error, null); same(result, Box.last);
                        if (mode == 0) {
                            same(((Box)result).value, data);
                            if (result == sentinel || result == cached) throw new AssertionError("original allocation disappeared");
                            if (data != null && data.length != 0) { ((Box)result).value[0] = 11; if (data[0] != 11) throw new AssertionError("array alias changed"); }
                        } else if (mode == 1) { same(result,cached); same(cached.value,cacheData); }
                        else same(result,null);
                    }
                }
                value(true);
            }
        for (int route = 0; route < 6; route++) {
            int before = Box.calls;
            try { call(route,null); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException expected) { value(true); failures++; }
            state(Box.calls == before);
        }
        for (byte[] data : arrays) {
            Core self = new Core(data); int[] marker = {0}; self.marker = marker; int before = Box.calls;
            same(Caller.read(self),data); state(self.calls == 1 && marker[0] == 3 && Box.calls == before);
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("original-array-target-and-factory=retained,mutable-aliases-and-failure-order=retained,complete-parent-and-interface-arguments=retained,typed-entry-and-forwarding-frame=reported");
    }
}
