import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Modifier;
public final class UnwrittenFinalHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("static default, field flags or initialization effects changed");
    }
    static int read(int route) throws Throwable {
        if (route == 0) return Client.read();
        if (route == 1) return UnwrittenFinalCaller.read();
        if (route == 2) return Owner.class.getField("zeroI").getInt(null);
        if (route == 3) return (int)MethodHandles.lookup().findStaticGetter(Alias.class, "zeroI", int.class).invokeExact();
        return FreshFinalCaller.read();
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean encoded = args[2].equals("true");
        Object seed = new Object(), untouched = new Object();
        Trace.seed = seed; Trace.first = untouched; Trace.second = untouched;
        Trace.failure = new RuntimeException("owned static default failure"); Trace.failAt = mode;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            int result = -1; Throwable failure = null;
            try { result = read(route); } catch (Throwable error) { failure = error; }
            long firstEvents = mode == 1 ? 1 : mode == 2 ? 142 : mode == 4 ? 14 : mode == 5 ? 1424 : 14243;
            require(Trace.events == (attempt == 0 ? firstEvents : 0));
            require(Trace.bad == 0 && Trace.observations == (mode == 1 ? 0 : mode == 2 || mode == 4 ? 1 : 2));
            require(Trace.first == (mode == 1 || mode == 4 ? untouched : null));
            require(Trace.second == (mode == 0 || mode == 3 || mode == 5 ? seed : untouched));
            if (mode == 0) require(failure == null && result == 0 && Client.anchor() == seed);
            else if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
            else require(failure instanceof NoClassDefFoundError);
            for (java.lang.reflect.Field field : Owner.class.getFields())
                if (field.getName().startsWith("zero")) require(Modifier.isFinal(field.getModifiers()) == encoded && Modifier.isStatic(field.getModifiers()));
            Trace.seed = new Object();
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
