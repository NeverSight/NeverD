package fixture;
public class Prefix {
    static { Trace.step(14); Trace.holder.value = Trace.providerValue; Trace.step(15); }
    static Object readPackage(Holder holder, Object token) { return Trace.read(holder, token); }
    protected static Object readProtected(Holder holder, Object token) { return Trace.read(holder, token); }
}
