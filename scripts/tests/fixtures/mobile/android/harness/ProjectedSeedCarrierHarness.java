import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.*;
public class ProjectedSeedCarrierHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned projected seed contract"); }
    static void call(int route, Object carrier, Object value, Callback<Object,Object,Object> callback) throws Throwable {
        if (route == 0) Core.call(carrier, value, callback);
        else if (route == 1) ProjectedSeedCarrierCaller.call(carrier, value, callback);
        else if (route == 2) {
            try { Core.class.getMethod("call", Object.class, Object.class, Callback.class).invoke(null, carrier, value, callback); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Core.class, "call",
                MethodType.methodType(void.class, Object.class, Object.class, Callback.class)).invoke(carrier, value, callback);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method send = Carrier.class.getMethod("send", Object.class, Object.class, Callback.class);
        ParameterizedType type = (ParameterizedType)send.getGenericParameterTypes()[2];
        Type lower = ((WildcardType)type.getActualTypeArguments()[0]).getLowerBounds()[0];
        if (args[2].equals("input")) require(lower.equals(Carrier.class.getTypeParameters()[0]));
        else require(lower.equals(Object.class));
        require(((WildcardType)type.getActualTypeArguments()[1]).getLowerBounds()[0].equals(send.getTypeParameters()[0]));
        Trace.failure = new IllegalArgumentException("owned seed failure");
        Trace.fail = mode <= 3 ? mode : 0;
        int[] data = {7}, calls = {0};
        Object carrier = mode == 5 ? null : new Object();
        Object value = mode == 6 ? null : data;
        Object result = mode == 7 ? null : new Object();
        Object[] last = {null,null};
        Callback<Object,Object,Object> callback = mode == 4 ? null : (first,second) -> {
            require(first == carrier && second == value); calls[0]++; last[0]=first; last[1]=second;
            Trace.hit(2); if (second != null) ((int[])second)[0]++; return result;
        };
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Object prior = new Object(); Trace.result = prior;
            int oldCalls = calls[0], oldData = data[0];
            Throwable thrown = null;
            try { call(route,carrier,value,callback); } catch (Throwable failure) { thrown=failure; }
            boolean entered = mode != 1 && mode != 4;
            boolean stored = entered && mode != 2;
            long expected = stored ? 123 : entered ? 12 : 1;
            if (mode >= 1 && mode <= 3) require(thrown == Trace.failure);
            else if (mode == 4) require(thrown instanceof NullPointerException);
            else require(thrown == null);
            require(Trace.events == expected && calls[0] == oldCalls + (entered ? 1 : 0));
            require(Trace.result == (stored ? result : prior));
            require(data[0] == oldData + (stored && value != null ? 1 : 0));
            if (entered) require(last[0] == carrier && last[1] == value);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
