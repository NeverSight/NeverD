import fixture.Core;
import fixture.Envelope;
import fixture.Provider;
public final class NestedConditionalTailCaller {
    public static Envelope call(Provider provider, long bits) { return Core.make(provider, bits); }
}
