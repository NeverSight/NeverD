import fixture.Core;
import fixture.Trace;
import java.util.Iterator;
import java.util.function.Supplier;
public final class DependentSeedBoundCaller {
    public static final class Box implements Iterable<String> {public int marker;public Iterator<String> iterator(){Trace.iterators++;throw new AssertionError("unexpected owned iteration");}}
    public static Box context(Box value){return Core.context(value);}
    public static Box throwing(Box value,Supplier<RuntimeException> errors){return Core.throwing(value,errors);}
}
