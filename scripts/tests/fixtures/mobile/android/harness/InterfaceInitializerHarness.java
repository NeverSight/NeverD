import fixture.*;
import java.lang.invoke.MethodHandles;
public final class InterfaceInitializerHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("interface initializer order"); }
    public static void main(String[] args) throws Throwable {
        int mode = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        State.failAt = mode;
        RuntimeException original = new IllegalArgumentException("owned initialization failure");
        State.failure = original;
        Class<?> api = Class.forName("fixture.Api", false, InterfaceInitializerHarness.class.getClassLoader());
        String[] fields = {"Z_FIRST", "A_SECOND", "COPY"};
        require(State.events == 0);
        for (int attempt = 0; attempt < 2; attempt++) {
            for (int slot = 0; slot < fields.length; slot++) {
                try {
                    Object result = route == 0 ? Caller.read(slot) : route == 1 ? InterfaceInitializerCaller.read(slot)
                        : route == 2 ? api.getField(fields[slot]).get(null)
                        : MethodHandles.lookup().findStaticGetter(api, fields[slot], slot == 2 ? Object.class : String.class).invoke();
                    require(mode == 0);
                    if (slot < 2) require(result.equals(slot == 0 ? "owned.first" : "owned.second"));
                    else require(result == Shared.VALUE && result != null);
                } catch (Throwable failure) {
                    require(mode > 0);
                    if (attempt == 0 && slot == 0)
                        require(failure.getClass() == ExceptionInInitializerError.class && failure.getCause() == original);
                    else require(failure.getClass() == NoClassDefFoundError.class);
                }
                require(State.events == (mode == 1 ? 1 : mode == 2 ? 12 : mode == 3 ? 123 : 1234));
            }
        }
        System.out.println("mode=" + mode + ",route=" + route + ",trace=" + State.events);
    }
}
