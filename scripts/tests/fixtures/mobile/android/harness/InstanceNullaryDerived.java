import fixture.Pair;
import fixture.Provider;
import fixture.Trace;
public final class InstanceNullaryDerived extends Provider {
    @Override public <K, V> Pair<K, V> make() {
        Trace.hit(5);
        return super.make();
    }
}
