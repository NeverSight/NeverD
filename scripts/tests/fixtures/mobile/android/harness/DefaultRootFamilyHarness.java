import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class DefaultRootFamilyHarness {
    public static void main(String[] args) throws Throwable {
        MangledInterfaceHarness.main(args);
        int route = Integer.parseInt(args[1]);
        String name = args[3];
        MangledInterfaceHarness.require(Api.class.getMethod(name, Object.class).isDefault());
        Branch<Object> receiver = new Branch<Object>() {};
        RuntimeException original = new RuntimeException("owned default body failure");
        Object value = new Object();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0;
            Trace.failAt = 0;
            Trace.failure = original;
            Throwable failure = null;
            try {
                if (route == 0 || route == 2) NamesKt.call(receiver, value);
                else if (route == 1) MangledInterfaceCaller.inherited(receiver, value);
                else if (route == 3) {
                    try { Api.class.getMethod(name, Object.class).invoke(receiver, value); }
                    catch (InvocationTargetException error) { throw error.getCause(); }
                } else MethodHandles.publicLookup().findVirtual(Api.class, name,
                        MethodType.methodType(Object.class, Object.class)).invoke(receiver, value);
            } catch (Throwable error) { failure = error; }
            MangledInterfaceHarness.require(failure == original && Trace.events == 6);
        }
    }
}
