package probe;
import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.reflect.*;
public final class InstanceArrayCarrierVoidCaller {
    private static Provider receiver() { Trace.hit(7); Trace.receiverAction.run(); return Trace.provider; }
    private static Payload[] array() { Trace.hit(8); Trace.arrayAction.run(); return Trace.values; }
    private static Contract.Creator<Payload> creator() { Trace.hit(9); Trace.creatorAction.run(); return Trace.creator; }
    public static Owner call(int route, Constructor<Owner> constructor, MethodHandle handle) throws Throwable {
        Provider provider = receiver(); Payload[] values = array(); Contract.Creator<Payload> creator = creator();
        if (route == 0) return new Owner(provider, values, creator);
        if (route == 1) return Owner.create(provider, values, creator);
        if (route == 2) {
            try { return constructor.newInstance(provider, values, creator); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Owner) handle.invokeExact(provider, values, creator);
    }
}
